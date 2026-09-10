# Architecture

A contributor-orientation map of how the two DLLs fit together. For the *what it does*
feature list see [`capabilities.md`](capabilities.md); for the deliberately-faithful quirks see
[`fidelity.md`](fidelity.md).

## The two DLLs and the boundary between them

The clone is a **backend** (`XINPUT1_3.dll`, built from `src/backend/`) and a **frontend**
(`snapmap-plus-ui.dll`, built from `src/ui/`).

- The backend loads first. DOOM loads `XINPUT1_3.dll` at startup (it sits in the game root and
  forwards the real XInput exports through to System32). Once running, the **backend** does
  `LoadLibraryA(".\\snapmap-plus\\snapmap-plus-ui.dll")` and then `CreateThread(sh_ui_init, ...)` to
  bring the frontend window up on its own thread.
- The frontend never touches the engine directly. Every engine read or write the UI needs goes
  through a shared **interface object** that the backend creates and hands to the frontend's
  init thread.

This split is the version-portability story: all the build-specific engine offsets and
signature-resolved engine calls live **behind the interface, in the backend**. The frontend
holds no raw engine addresses, so a DOOM update only forces a re-derive on the backend side.

## The frontend: a WebView2 (HTML) window

The frontend (`src/ui/webview/snapmap_plus_ui_webview.cpp`) hosts the Snapmap+ UI as HTML/CSS/JS in a
Microsoft Edge **WebView2** control inside a plain Win32 window. Its `sh_ui_init` entry (export
ordinal 10, the same entry the backend calls) creates the window, brings up WebView2, loads the UI
(`mockup.html`, compiled into the DLL), wires the JS <-> native bridge, stores the backend **interface**
pointer, then enters the think-loop and never returns. The UI's structure — the tabs, the entity list,
the entity-state editor, the timeline editor, prefabs — lives in the HTML; the C++ host is a thin bridge
that turns JS messages into interface-slot calls and posts results back to the page. Full detail:
[`webview-ui.md`](webview-ui.md).

Theme selection is available before the first navigation: the host reads the registered `theme` setting,
adds `class="dark"` to the embedded document root when needed, and only lets the native window become
visible after a successful `NavigationCompleted`. A returning dark-theme user therefore never sees a
light or blank first frame.

The host keeps a normal `WS_OVERLAPPEDWINDOW` so Windows still owns resizing, Aero Snap, minimize,
maximize, and taskbar behavior, then consumes `WM_NCCALCSIZE` so the HTML menubar replaces the visible
caption. A one-pixel `DwmExtendFrameIntoClientArea` margin preserves DWM's rounded Windows 11 corners and
drop shadow on that captionless client, matching snapmap-midi without switching to a behavior-poor
frameless-window style.

## The asset browser is a live installed-data view

The asset browser does not ship or build a second asset library. Selecting a category posts that one
kind through the existing interface, the host pages newline-delimited names from the backend, and the
page keeps a two-category least-recently-used cache -- the minimum that lets the tab and modal retain
their independent current categories without accumulating every list visited. Scalar counts remain
for the rail. Materials and Sounds request one extra qualifier list for atlas-only rows and soundbank
names respectively.

The backend parses the game's installed resource indexes only when the first catalog or plain-image
preview needs them. It retains interned names plus resource-file offsets and releases the raw index
documents; record classes from the broader base-game box that cannot serve a list or preview route
are discarded while parsing. Wwise event/bank metadata and the `.vmtr` name union have separate
first-use gates. The Wwise XML is streamed for only bank and event tags, and VMTR names occupy an
exact string pool rather than fixed-width slots. Event, bank-row, and decl-less-material pointer
tables are also shrunk to their final deduplicated counts.

A preview then seeks to the selected payload, reads and decodes that payload in memory, and publishes
only the resulting thumbnail. Mega2 page lookup follows the same rule: one 4-byte page id and one
16-byte offset/size entry are read for a selected cell instead of copying whole shard tables. The
worker sleeps between requests, allocates decode scratch only for an atlas-backed preview, and
releases it after an idle interval. Encoded previews are consumed by the page rather than retained
on both sides of the interface. Image selections carry their catalog kind through the existing
append-stable request slot, so a direct Image bypasses VMTR and cannot be captured by a same-named
Material record. The large game files remain the source of truth and are never copied into the
overlay.

## The prefab preview reconstructs an installed-data scene

Prefab Details does not capture the game's renderer or build a screenshot. The host reads only the
currently selected prefab JSON and sends it to the page, where `prefab_viewport.js` reconstructs entity
transforms from the prefab-local `spawnPosition`, `spawnOrientation`, and scale values. The serialized
orientation is a sparse patch over identity: each `mat[n]` is one complete local axis, matching idTech's
column-major `idMat3`, and an omitted component retains its identity value. Scale is sparse too, but its
base is the inherited entityDef's `renderModelInfo.scale` rather than always `{1,1,1}`. Direct
`renderModelInfo.model` names are already in the file; otherwise the backend first uses the pure,
read-locked entityDef lookup without loading or creating an engine object. A file-only fallback follows
installed `snapEditorEntityDef` / `entityDef` inheritance and composes the first derived occurrence of
each scale component. That fallback also understands
`spawnerEntityPair.entityStatic`, so pickup spawners resolve to the armor, health, ammo, or equipment mesh
they represent instead of becoming generic boxes.

Geometry crosses four append-only interface slots: resolve inherit to model (`+0x308`), enqueue an
installed mesh request (`+0x310`), consume one completion (`+0x318`), and resolve the model plus inherited
scale defaults (`+0x320`). The original model-only slot remains intact for paired-version compatibility.
A single bounded worker uses the
asset browser's lazy installed-resource index to seek the requested BMODEL or MD6 payload. It decodes only
positions, packed normals, and indices, with hard source/vertex/index/surface limits. BMODEL's fixed
32-byte per-surface metadata is consumed between surfaces; treating that block as the next material header
was the reason formerly working single-surface geometry degraded every multi-surface prop to a proxy. The frontend host
moves one completion per think-loop tick through a WebView2 shared buffer, avoiding base64 expansion; the
page uploads it and immediately releases the shared buffer.

The page classifies saved entities before drawing them. Props and resolved pickup spawners use their real
installed mesh; blockers prefer the visible `renderModels` shell rather than its editor trigger shell;
SnapMap logic, action/listener I/O, and filter entities use the installed hexagon, circle, and diamond
editor meshes. Hexagons retain their full editor size while I/O circles and filter diamonds use the
editor's half scale. The common saved `isVisible: false` state is not treated as a trigger classification;
class/inheritance semantics keep ordinary props, pickups, and logic nodes solid. Actual invisible triggers
are faint outlined helpers and do not control automatic framing. Decals
remain thin helper planes because their appearance is texture data, not geometry. Only truly unsupported,
over-budget, missing, or transport-incompatible solid geometry falls back to a procedural box. Block and
trigger fallback boxes match the installed unit meshes' bottom origin instead of centering around the spawn
point. Neutral lighting uses an inverse-transpose normal matrix, so strongly non-uniform block dimensions
do not skew their shading. The floor
keeps Cartesian square coordinates but extends beyond the scene and fades through a circular radial mask.

This is a read-only hook into files the player already installed, not a shipped asset library. Snapmap+
does not persist or package payload bytes, textures, materials, skeletons, animations, or game renderer
state. WebGL draws only when input, geometry, theme, or the observed preview bounds change. Its device-pixel
ratio, triangle count, and GPU cache are bounded, so resizing the native window or the shared pane divider
does not create a continuous render loop.

## The 30 Hz manual think-loop

The frontend runs its own pump (the same shape as OG `FUN_180015c04`), once per frame at roughly 30 Hz,
under a loop mutex — draining the backend work-queue rather than relying on any UI toolkit's event loop:

```
lock(loop_mutex)
    (*(interface + 0x1a0))()      // drain the backend work-queue: run queued {handler, args}
    apply deferred UI-driven writes (snapshotted in the JS message callback)
unlock
sample the live camera origin when the editor is visible and not position-locked
pump the window's messages
Sleep(33ms)                       // ~30 Hz
```

This is **load-bearing**, not a stylistic choice. The frontend's engine-touching requests
(Save-to-Decl, timeline commits, prefab staging) are snapshotted off the re-entrant JS message
callback and issued here, from this one worker thread (a WebView2-callback or RPC-thread engine call
deadlocks the engine's command-system lock). Replicate the pump. Two thread facts to keep straight:

- The think-loop thread is the **frontend's own worker** (`CreateThread` in `ui_bridge.c`), **not
  DOOM's main thread** — the engine treats it as foreign (issue #61). Decl-edit commits issued from
  it (`apply_sync` `+0x290`) are marshaled by the backend onto the engine's thread through the
  `clone_bss_apply` command-buffer drain, with the caller blocking (normally one frame) for the
  synchronous applied count. If the main-thread identity or marshal transport
  is unavailable, the current implementation retains an inline compatibility
  fallback; it is not an unconditional main-thread guarantee.
- The SnapStack subcommands no longer run on this drain at all: the `sh` console dispatch executes
  them inline on DOOM's main thread at the engine's command-exec point (see
  [`fidelity.md`](fidelity.md)'s sanctioned divergence). The `+0x1a0` drain still runs every tick —
  it carries the backend's per-tick hook — but its queue ships producer-less.

The live camera-origin read intentionally runs at this full cadence rather than inside the separate
10-frame (~330 ms) entity-list/selection/state poll. It posts to WebView2 only when a coordinate changes,
so stationary-camera frames pay for one guarded vec3 read but enqueue no page message.

## Editor navigation snapshots and rendering

The existing frontend tick schedules at most one navigation refresh per second.
The engine command drain performs the complete edit-map serialization on the
game thread. A thread-local snapshot guard suppresses save embedding and shadow
writes during this read. Geometry, module transforms and `instanceEntities`
ownership are validated and published together under the bake lock; a changed
snapshot invalidates cached generated areas. No new interface slots are needed.

The edit-to-build detour takes a final snapshot and freezes its revision while
the engine composes the three ground-monster AAS classes. Two verified BuildAAS
call sites select temporary resource names containing the revision and exact
module-instance index. The ordinary engine transform, merge and temporary
resource destruction paths then handle each independent payload.

Object Mode renders the validated monster48 preview. The game thread publishes
a private double-buffered line list; native render-stage and post-process hooks
match it to the current renderer-side world and view. Renderer consumers hold a
shared lock and never read mutable editor entities. Blueprint Mode, unavailable
snapshots and editor teardown clear the display. See [navigation.md](navigation.md)
for geometry support, refresh timing and limits.

## Engine allocations inherit a heap scope — mind the lifetime

Any engine object we build through idlib containers is allocated from **whatever heap is currently on top
of `idMemLocal`'s heap-scope stack**, because idlib always asks `Mem_Alloc` for heap id `-1` ("current
scope"). The engine keeps three heaps — global/process, persist, and **map** — and the map heap is
`HeapDestroy`d at map load. So the lifetime of an engine allocation is decided by **when it runs**, not by
what is allocated, and while the SnapMap editor is up the ambient scope is the *map* heap.

Practical rule: **anything we build that must outlive the current map has to be allocated inside an
explicit `idMemLocal::PushHeap(0)` / `PopHeap()` pair.** Everything else is fine as-is — an object created
and destroyed within one call cannot outlive its heap, and objects that genuinely belong to the map
*should* die with it.

Prefab staging in `ae_mkcmd_one` and declaration commits in `ae_apply_one` use
an explicit process-heap scope because their allocations can outlive the map.
Temporary engine objects still require their matching destructors. See the
heap helpers and lifetime checks in `src/backend/apply_engine.c`.

Two properties of the mechanism worth knowing before using it:

- It is **main-thread-only.** `PushHeap`, `PopHeap` and `Mem_Alloc`'s `-1` lookup share a
  `GetCurrentThreadId()` gate; off the engine's main thread all three are silently inert.
- The scope stack is **global, not per-thread**, so a push briefly changes the ambient heap for other
  threads. That is a leak risk, never corruption — each block records its own heap in its header and
  `Mem_Free` reads it back, so a block is always freed into the heap it came from.

`PopHeap` **fatals on underflow**, so pushes and pops must be balanced across early returns and exceptions.

## The interface vtable (the matched-pair ABI)

The shared interface object is defined once, in `src/common/snapmap_plus_iface.h`, and **both DLLs
include that header** — it is a matched pair. The backend writes the vtable and fields; the
frontend reads them at the same offsets.

- The backend builds it (`operator_new(0x60)`), installs the vtable — the **77 original-faithful
  slots** (`+0x00..+0x260`) plus the **extension slots** appended after them (`+0x268..+0x338`,
  `sizeof(sh_iface_vtbl) == 0x340`: the atomic class+inherit apply, the class/inherit
  enumerators, the dev-layer query, the wire-edit generation counter, the synchronous `apply_sync`,
  the timeline inherit-normalize, push/clear-stack, the generic configuration getter/setter, the
  asset-browser group — preview request/publish, request-by-name, the material atlas rect, the
  catalog pager, sound preview/session, and prefab model, mesh and scale-default lookup, and the rawmap file trio
  `rawmap_status` (`+0x328`), `rawmap_configure` (`+0x330`) and `rawmap_load_now`
  (`+0x338`), which back the File menu's Load Rawmap / Save Rawmap As) — initializes the mutex at `+0x08`, and hangs a
  sub-object off `+0x58` that holds the SnapStack subcommand map and the work-queue (drained by the
  frontend's worker thread; producer-less since the `sh` dispatch moved inline onto the engine's
  thread — see "The 30 Hz manual think-loop").
- **Extension slots are append-only**: a new capability gets the next slot after the current end;
  original-block offsets never move. This is also a real failure mode, not a formality — a frontend
  calling an extension slot that an older backend never installed would call through garbage. That is
  why `build.ps1` builds both DLLs from the same header in one pass by default (its `-BackendOnly`
  switch skips only the frontend — the safe direction, since an older frontend never reads past a
  newer backend's vtable), and why the frontend null-probes an extension slot (falling back or
  skipping the feature) rather than assuming it.
- The frontend calls vtable slots for everything it needs from the engine: entity
  count/validity, classname/inherit/displayname read and write, serialize/deserialize an
  entity, apply an edit (`+0xd0`), enqueue and drain the work-queue (`+0x90` / `+0x1a0`),
  register/unregister SnapStack subcommands (`+0x188` / `+0x190`), enumerate decls, manage the
  selection, show toasts (`+0x1b8`), and read/write registered settings as JSON fragments
  (`config_get_json` `+0x2B0` / `config_set_json` `+0x2B8`).

Because this vtable is the *clone's own* ABI — not a DOOM structure — it is self-consistent and
not DOOM-build-dependent. The only hardcoded offsets that cross the DLL line are these vtable
slot offsets and the `WIN[...]` field offsets. **They must stay pinned identically in both
DLLs**; the two are a matched set. The build-specific *engine* offsets sit behind the vtable in
the backend, where they are re-derived per build.

## Persistent configuration

Two files, owned by different sides on purpose. The backend owns `config.json` — the registered
settings, validated and versioned. The **frontend host** owns `pinned.json`, the asset browser's
shortlist, and deliberately keeps it out of the settings registry: `config.json` is all-or-nothing,
so any parse failure resets the whole document to defaults, which is an acceptable trade for a
handful of validated scalars and not for unbounded data a user grows themselves. The host moves those
bytes and parses none of them; shape and validation live in the UI, the only side that knows what a
pin means. See [`capabilities.md`](capabilities.md#persistent-settings).


```json
{
  "schema_version": 1,
  "settings": {
    "theme": "light",
    "entities.show_hidden": false,
    "entities.selection_mode": "off",
    "overrides.user_enabled": true,
    "packages.embed_in_saved_maps": true,
    "navmesh.enabled": true,
    "navmesh.preview": true,
    "navmesh.embed_in_saved_maps": true
  }
}
```

Deleting the file deliberately is therefore a clean reset: the next startup, or the next setting write
in a running session, recreates it. Missing or deleted configuration restores `overrides.user_enabled` to
enabled. Manual config edits are consumed at the next startup; a successful `sh_user_overrides 0` or
`sh_user_overrides 1` write goes through the existing setter and so recreates a deleted file. The one descriptor
table in `src/backend/config.c` declares each setting's key, JSON type, default, validator/normalizer, and
backend/frontend read/write permissions. In addition to `theme`, the registry has the
`entities.show_hidden` boolean, `entities.selection_mode` enum (`off`, `follow`, or `select_in_3d`),
`overrides.user_enabled` boolean (true by default), and the booleans
`packages.embed_in_saved_maps`, `navmesh.enabled`, `navmesh.preview` and
`navmesh.embed_in_saved_maps` (all true by default); the schema version and generic backend↔frontend ABI
are unchanged. Add a descriptor, behavior and validation when adding a setting;
the wire contract remains generic.

Values cross the matched-pair ABI as complete UTF-8 JSON fragments. `config_get_json` at `+0x2B0`
supports a size query and reports status flags; `config_set_json` at `+0x2B8` validates the registered
key/value and returns rejected, persisted, or session-only. The WebView host exposes those calls to the
page as generic `configGet` / `configSet` messages carrying `valueJson`. This accommodates future
booleans, numbers, strings, arrays, and objects without growing the ABI once per setting; the generic
bridge already permits a future frontend control for user overrides, though none exists today.

The parser accepts an optional UTF-8 BOM, caps the file at 64 KiB, rejects malformed UTF-8, malformed
JSON, excessive nesting, and duplicate object keys, and requires the supported schema version. For a
supported document it repairs missing or invalid registered values to their defaults while preserving
unknown members under both the root and `settings`. A malformed, structurally invalid, or oversized file
is moved to a timestamped `config.<timestamp>[.<collision>].corrupt.json` backup and replaced with
defaults; the UI warns once for that startup. A document with a newer schema version is instead left
byte-for-byte untouched: the current process uses defaults and refuses to overwrite preferences it does
not understand.

Writes are serialized by an in-process lock and a local-session named mutex. A setter rereads the file
while holding that mutex so it does not discard an external writer's unknown values, writes and flushes a
same-directory temporary file, then atomically replaces `config.json`. Existing-file replacements use
paired temporary/rollback names; if a process stops in Windows' documented partial-replacement state,
the next startup recognizes the pair and restores the prior file before applying missing-file reset
semantics. Creation, read, write, flush, backup, replacement, or mutex failures leave the last good
on-disk file intact where possible and switch the affected value to session-only memory with a visible
warning. `overrides.user_enabled` is the exception to that general session-only behavior: its immutable
launch snapshot has already been captured, so a failed `sh_user_overrides` write reports that it was not
saved, leaves this launch unchanged, and establishes no next-launch change. The two-DLL overlay and
installer payload are unchanged; update/uninstall/reinstall preserve this runtime-owned file.

### Startup ordering

The backend initializes configuration and captures the user-overrides setting
before publishing resource providers. It then captures installed resources and
declaration sources and arms native registration before boot resource promotion:

```text
config -> user-overrides snapshot -> resource shadow -> installed resources
       -> command system -> combined decl snapshot -> boot-promotion hook
       -> apply and drain requirements -> register decls -> original promotion
```

The override setting is a launch choice; changing it does not replace providers
already serving the current process.

## Override packages

An override package is one directory below `overrides/` holding its own content, marked by a `package.json`:

```
overrides/cyberdemon/package.json
overrides/cyberdemon/decls/<type>/<logical-name>.decl
overrides/cyberdemon/resources/<name>.manifest
overrides/cyberdemon/requirements/<name>.requirements
overrides/cyberdemon/strings/<name>.json
overrides/cyberdemon/hud/weapons.json
overrides/cyberdemon/shaders/generated/spirv/<name>.{vspv,fspv,cspv}
overrides/cyberdemon/shaders/generated/renderprogs/<name>_pc_vulkan.bin
```

Install a package by copying its folder into `overrides/`; remove it by deleting
that folder. Consumers translate package paths into engine resource names without
building a merged archive. Manual changes take effect on the next launch. The
map-package installer can explicitly re-arm supported consumers during a session;
removing files does not undo objects already registered in the running engine.

A directory *without* a `package.json` is not a package -- it is a grouping folder, and the search continues
inside it. Users can therefore organise their installs to any depth (up to 8) without anything being compiled:

```
overrides/editor/lifts/package.json          -> package "editor/lifts"
overrides/editor/toybox/package.json         -> package "editor/toybox"
overrides/demons/hell/imps/package.json      -> package "demons/hell/imps"
```

A package is a leaf; the search never descends into one, so a package cannot contain another and its own
subdirectories always mean what the layout above says. Inside `decls/` the path *is* the decl's identity
(`decls/<type>/<logical-name>.decl`), so extra organisation belongs in the grouping folders above a package,
not inside it. A package's identity is its full path below `overrides/`, so two groups may hold like-named
packages. Scratch and notes folders are safe to keep alongside; `shader_includes/` is reserved for the file
shadow and is never searched. Packages are read by descending `package.json` priority (default 0), then
case-insensitive name, and an enumeration that could not complete -- unreadable subtree, over 64 packages, deeper than 8 --
returns failure. Decl and resource capture reject incomplete enumeration. File
shadows, string injection and map-package boot capture can retain enumerated
subsets, so their inventories may disagree with registration availability.

### Packages compose; only disagreements are refused

Packages can share prerequisites. Each consumer applies its own composition rule:

| Layer | Two packages ship the same thing | They ship *different* things under one name |
|---|---|---|
| Decls | Byte-identical decls compose: the first copy serves the identity, the rest collapse (`decl-server COMPOSED`) | Refused, naming the packages that disagree |
| Resource manifests | Identical rows compose into one served entry | Refused, naming both provider rows |
| Requirements | Identical requests compose into one allowlisted cvar assignment per snapshot | A different value for an allowlisted name is refused |
| Strings | Identical text for one `#str_` id composes into one row | Refused, naming both packages; the first definition stands |

These composition rules apply to published identities and policy tables.
Existing-file shadows use package priority instead: the first matching package
serves the file. The overlap reporter names differing copies and their selected
winner. A loose root override precedes package shadows. HUD policy conflicts
refuse the complete HUD table; they do not choose by priority.

Requirements are owned by the package, not by Snapmap+. Snapmap+ ships only a tiny allowlist of settings a
package is *permitted* to ask for; it sets nothing on its own. The cut-content blacklist cvars the Cyberdemon
needs live in `overrides/cyberdemon/requirements/cyberdemon.requirements`, so uninstalling that package removes
the request with it.

The pre-package layout -- a single shared `overrides/generated` tree -- is no longer a package. Installing or
updating migrates it into `overrides/my-overrides`, a real package with its own marker, and a fresh install
gets that folder empty as a place for user content. Migration copies missing files
and keeps existing destinations. Its source-removal check tests destination path
presence, not byte equality, and ignores enumeration errors. A same-name collision
can therefore discard different source content. Loose root shadows remain a
separate lookup path from package resolution.

### Weapon HUD policy

Packages may declare exact weapon-to-ammo-display mappings in `hud/weapons.json`.
The bounded, strictly parsed table is captured at startup and during package
re-arm; identical requests compose and disagreements refuse the HUD table.
One portable, verified ammo-widget call uses the table without modifying game
mode, ammunition or declarations. It preserves engine behavior for unlisted
weapons and when overrides are disabled or validation fails.
See [weapon-hud.md](weapon-hud.md) for the schema, lifecycle and client limits.

### The file shadow resolves across packages too

The decl server publishes an identity, but the bytes the engine parses come from the file shadow, and the
engine only ever asks for a decl by its canonical virtual name -- `generated/decls/<type>/<name>.decl`. Before
packages that mapped one-to-one onto `overrides/generated/decls/...`, so joining the requested name onto the
overrides root *was* the resolver. A package owns its own root, so that join can never reach it: DOOM has no
idea `overrides/cyberdemon/` exists and will never ask for `cyberdemon/decls/...`.

So a request is resolved against the overrides root first -- `overrides/<engine name>`, the plain file shadow,
which serves ANY engine resource and is how a loose file dropped in the overrides root still works -- and
then, for the namespaces in the table above, against each installed package as `<package root>/decls/<rest>`,
in the same deterministic order. Declaration requests reach only the package's
`decls/` subtree; shader requests use the separate mapping below. Package metadata
such as `package.json` is not exposed as an engine resource. The file-open path
uses a captured package set, refreshed by the explicit runtime re-arm operation.

Without this the failure is silent and total: the identity registers, the engine opens nothing, and the parse
yields an empty default -- which for a `snapEditorEntityDef` means no resolved `entityDef`, a rejection by the
native palette validator, and a terminal materialization failure that refuses every decl behind it.

The same resolution serves a package's shaders. A render program is an ordinary decl type, so its
`decls/renderprog/<name>.decl` needs nothing new; but the compiled module is opened separately, by the engine
name `generated/spirv/<name>.{vspv,fspv,cspv}`, and its pre-translated source blob as
`generated/renderprogs/<name>_pc_vulkan.bin`. Both go through the same provider slot this layer hooks, with a
mode the hook admits. Under `shaders/` the package path mirrors the engine name verbatim, so a package can
carry a new render program. Packages that reuse a compiled resource name compete
under the same priority-based file-shadow rule.

Only the declaration and compiled-shader namespaces described above are package-resolved,
each within its designated subtree.
Everything else a package contains stays unreachable to the engine.

### A package names its own content

The editor resolves an entity's `displayNameTag` through `idLangDict`. Packages
can supply their own `#str_` text without modifying the user's global
`strings/strids.json`.

A package now carries `strings/<name>.json`, a flat `{ "id": "text" }` map, injected on the engine's first
string-table sort. The order is the user's own document, then every installed package, then Snapmap+'s baked
defaults: the user's explicit value still outranks a package's, and a package shipping a key we also bake is
deliberately replacing our fallback. A key is never appended twice -- a duplicate corrupts the engine's
sorted-by-hash dictionary and makes lookups collapse onto the wrong text.

Prefer an id the game already ships when one exists: it is localized in every language the game supports,
where a string a package carries is only as translated as its author made it. The Cyberdemon package uses
the game's own codex text for that reason.

## Existing shadows versus genuinely new decls

These services share the launch user-overrides setting and data root:

| Service | Input | Result |
|---|---|---|
| File shadow | A requested resource name | Serves a loose or package file before the installed resource. |
| Installed resource bridge | Package `resources/*.manifest` files | Reads selected resources from the user's installed archives on demand. |
| Dynamic decl server | Package `decls/<type>/<name>.decl` files and linked decls | Registers absent identities through exact `decltree` sources, materializes objects and requests a palette rebuild. |
| Package requirements | Package `requirements/*.requirements` files | Applies allowlisted cvar values before decl publication. |

### Startup and runtime registration

At startup, `sh_decl_server_install` captures a combined decl snapshot, resolves
the native boundaries, registers its internal commands and arms a hook on the
engine's whole-registry promotion (`ResourceStaticPromote`). Its first main-thread
entry applies package requirements, drains the command buffer, then runs
registration before calling the original promotion. That native pass gives the
new resources level 4 alongside shipped content, so later map purges do not free
them. Publication faults are contained and the original promotion still runs.
An after-promotion lookup logs the level of one published identity.

Registration does not wait for `RUNNING`: that point is after boot promotion.
The requirements service retains a `RUNNING` polling fallback when its snapshot
has not already been applied. Requirements are `cvar<TAB>name<TAB>value` rows;
the allowlist currently permits only the two cut-content blacklist gates at
value `0`. Equal requests compose. Invalid, conflicting, reparse-backed or
unexpectedly unreadable input refuses that requirements snapshot. A requirements
failure is logged; it can leave the native loader unable to materialize gated
content.

The map-package installer can request an explicit runtime rearm. An atomic
request is consumed on the next main-thread tick; the internal rearm command
uses the same synchronous path. It refreshes package discovery, linked-resource
manifests and weapon HUD policy, then recaptures and drains requirements. It
reopens decl publication, captures fresh candidates and merges new exact sources
over the previously published table. Existing source allocations remain alive
for streams that may still reference them.

Runtime registration brackets the pass with a registry watermark and promotes
new resources to level 4 afterward. Materialization also promotes reused
objects, which predate that watermark. It marks eligible empty placeholders and
newly served live shadows before any reload, then drains through native lookup
or the generic teardown/load fallback. Previously admitted identities are left
alone. Undrained pending-load marks are cleared before the next map transition.
Rearm is intended for the browser with no map loading; it must not repeat the
boot-wide promotion, which would also retain unrelated map resources.

This is an explicit rescan, with no filesystem watcher. A completed or failed
decl pass can be rearmed through the command/request path; an in-flight pass is
refused. The direct `sh_decl_server_rearm` API accepts only `DONE`. Component
refusals still matter: a refused palette builder remains refused for the process,
and package strings retain their separate one-shot startup injection.

Two current limits matter when extending this lifecycle. An empty launch
snapshot returns before the decl server binds native dependencies or registers
its commands. An all-shadowed pass returns before materialization, live-shadow
reload and palette refresh. Runtime rearm therefore does not guarantee every
consumer has refreshed after an arbitrary package change.

### Discovery and native publication

Discovery combines local package decls with the bridge's linked `.decl` subset.
A local body takes precedence over a linked body with the same identity. Equal
local definitions compose; differing definitions are reported and refused.
Metadata is ordered case-insensitively by type, name and source before admission.
Discovery allows 4,096 paths, admits at most 512 identities, and limits each body
to 1 MiB and the admitted bodies to 16 MiB. Refused collision groups do not use
the admission quota; later body refusals do not backfill their slots.

Paths must stay within the package decl root and use the supported unquoted-token
alphabet. Reparse points, traversal, malformed paths, embedded NULs and bodies
without one balanced top-level block are refused. Allocation failure, discovery
overflow or unexpected directory/read errors reject the complete snapshot.
Missing directories and normal enumeration completion are allowed. Structural
checks do not replace DOOM's semantic parser.

Within the admitted set, an unambiguous logical name in a quoted value creates
a dependency edge. Referenced decls scan before their consumers; comments,
escaped values, ambiguous names and external identities add no edges. Independent
entries retain stable ordering, and cycles retain admission order.

The main thread validates the registry and classifies every candidate before
publishing bytes. It resolves the short type through registry slot `+0x58`, then
uses `DeclSourceFind`. An existing source is `SHADOWED`. Only a source miss calls
`DeclFind(..., makeDefault=0)` to check for a live object; existing objects are
also `SHADOWED`. Unsupported types are `REFUSED`, and absent identities are
`MISSING`. A native lookup exception stops the pass before publication.

The provider copies missing bodies into immutable entries keyed by
`decltree/<type>/<logical-name>.decl`. These exact entries precede loose files,
linked resources, built-ins and packaged bytes. A matching entry is authoritative:
failure to open it does not fall through to another same-named source. Runtime
merge preserves earlier keys, with new entries winning exact collisions.

For each missing candidate, the scanner receives a native 48-byte `idStr`
containing `<type>/<logical-name>.decl` through registry slot `+0x38`:
`DeclRegisterFile(registry, &source, NULL)`. The engine canonicalizes that name
under `decltree`, opens the supplied body and derives its identity from the path.
The temporary string is destroyed with `IdStrDtor`. A false scanner result or a
constructor, scanner or destructor exception stops the current pass. Already
published source storage remains available; native partial registration is not
rolled back.

### Materialization, visibility and palette state

All missing sources are scanned before materialization begins. Non-editor
identities materialize first in dependency order, followed by eligible new
`snapEditorEntityDef` roots. Lookup uses `makeDefault=0` first and creates only
an absent object with `makeDefault=1`. Missing managers or objects, unreadable
state at `+0x2c`, the in-progress bit `0x01`, or a native exception fail the pass.
The generic valid bit `0x04` is diagnostic rather than an admission requirement.

An editor root must lexically contain a real top-level `inherit` or direct
`edit.entityDef` assignment. Source-only abstract bodies are `NON-PALETTE` and
remain registered without materialization. Eligible roots must satisfy the
native palette contract: a resolved entityDef at `+0x1c8`, output targets carrying
flag `0x20`, and input targets carrying flag `0x10` at target offset `+0x3cd`.

After materialization, the server attempts the `decl_visibility` hook before
palette refresh. The engine consults the source catalog below map-load state 2,
but later probes `generated/decls` through the resource manager. The hook keeps
native successes and output arguments unchanged; it changes only a negative
boolean result for an exact published identity. Installation requires a clean
prologue match at the live vtable method and preserves all seven arguments,
including the trailing quiet flag. Without that flag, an ordinary cache miss
can become a fatal engine error. Installation also needs the diagnostic
owned/published candidate pair; refusal is logged without undoing registration.

`palette_refresh` then resolves the editor singleton, validates the live palette
vtable's read-only host location and calls
`SnapPaletteBuild(editor + 0x20660, NULL)`. It can rebuild after each registration
pass. Invalid dependencies, objects or native calls place this component in
terminal `REFUSED` for the process.

The decl server sets its explicit registration-success bit after source scans
and required materialization, even if palette refresh declines. Visibility and
palette outcomes must therefore be read separately when assessing map usability.
`DONE` alone is also insufficient: disabled, empty and all-shadowed snapshots
reach it without publishing new identities. There is no aggregate source alias,
per-identity `AddFromText`, raw object cache, `DeclFind` detour or live rollback.

### Installed resource bridge

A manifest row contains exactly three tab-separated fields: type, logical name
and installed virtual path. An empty path uses the logical name for pathless
records. The bridge resolves these against `gameresources.pindex`; multiple
provider paths may belong to one logical identity, but each provider path must
be unambiguous. Identical rows compose. Repeated exact pindex rows require
compatible metadata and identical stored payload bytes.

Capture validates paths, archive selectors, offsets, sizes and file bounds,
then retains selected metadata. It opens `gameresources.resources` and
`gameresources.patch` read-only and reads or decodes a slice when requested.
Limits are 64 first-level manifests, 1 MiB per manifest, 4 MiB total manifest
text, 4,096 resource entries, 64 MiB per resource and 256 MiB declared decoded
bytes per snapshot. Invalid or incomplete enumeration and metadata refuse
the capture; publication is `NEW -> INSTALLING -> READY`, or `FAILED`.

Explicit recapture reopens this state after package changes. Previous entry
allocations are retained for readers already inside a lookup, so repeated
recaptures accumulate memory. Callers must serialize this operation at a
quiescent boundary. The file-shadow package list similarly uses two atomically
selected buffers without tracking reader lifetime.

Compressed slices must be a complete raw-DEFLATE stream ending in BFINAL, or
the DOOM sync-flush form ending exactly at a non-final empty stored block
(`LEN=0`, `NLEN=0xffff`). The decoder requires zero alignment padding and rejects
trailing bytes, concatenated streams and invalid or truncated encodings. A
matched read/decode failure is an error, not permission to choose a different
same-named engine row. The bridge uses assets already in the player's installed
archives; it does not download or rewrite game resources.

## The override provider's idFile ABI

Returned streams implement the supported 31-slot `idFile` layout (`+0x00` through
`+0xf0`). The resource provider's separate open slot is `+0xf8`. Disk and memory
streams preserve native 64-bit read/write counts; memory streams are bounded
and read-only. `SetLength` at `+0x60` refuses every request. Drive/storage slots
report `+0xc0=0`, `+0xc8=true`, `+0xd0=0` and `+0xd8=0`; invalid or overflowing
memory seeks leave the cursor unchanged.

The final three stream slots (`+0xe0`, `+0xe8`, `+0xf0`) call native ReadString,
Compare and WriteString helpers for the engine's `idStr` ABI. All three must
resolve with clean `SIG_OK` status before the table and provider hook are
published. The constructor must also resolve cleanly; its first matching
RIP-relative LEA locates the provider vtable. Installation checks function
containment and requires the decoded vtable to lie in a read-only host section.
Missing, ambiguous or hook-tolerant results refuse installation.

Pinned RVAs are audit references. The implementation supplies the known stream
layout; it does not dynamically count a different engine build's virtual slots.
Signature uniqueness and address plausibility are compatibility checks, not
proof of semantic identity for an unknown revision. The decl registry has its
own independent checks: its decoded anchor must be clean, and live slots
`+0x38` and `+0x58` must equal the resolved registration and type-lookup methods.
