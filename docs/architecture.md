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
pump the window's messages
Sleep(33ms)                       // ~30 Hz
```

This is **load-bearing**, not a stylistic choice. Heavy engine work (the SnapStack apply chain,
Save-to-Decl, timeline commits) is snapshotted off the re-entrant JS message callback and applied here,
on the think-loop thread; the manual pump plus the `+0x1a0` work-queue drain *are* the frontend's
main-thread execution point (a UI-thread or RPC-thread engine call deadlocks the engine's command-system
lock). Replicate the pump.

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

Today exactly one thing we build outlives its call: the prefab staged into `editor+0x209a8` by
`ae_mkcmd_one`. Every other engine ctor in the backend is paired with its dtor in the same function. That
one site is scope-pushed; see [`backend-changes.md`](backend-changes.md) for the failure it caused before
it was, and doom-re `docs/truth/engine/memory-heaps-and-allocator.md` for the engine-side derivation.

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
  slots** (`+0x00..+0x260`) plus the **clone-extension slots** appended after them (`+0x268..+0x318`
  today, `sizeof(sh_iface_vtbl) == 0x320`: the atomic class+inherit apply, the class/inherit
  enumerators, the dev-layer query, the wire-edit generation counter, the synchronous `apply_sync`,
  the timeline inherit-normalize, push/clear-stack, the generic configuration getter/setter, and the
  asset-browser group — preview request/publish, request-by-name, the material atlas rect, the
  catalog pager, sound preview/session, and prefab model resolution/mesh transport) — initializes the mutex at `+0x08`, and hangs a
  sub-object off `+0x58` that holds the SnapStack subcommand map and the main-thread work-queue.
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

The backend is the sole owner of `%LOCALAPPDATA%\snapmap-plus\config.json`; the installer does not
generate, parse, or replace it. `sh_config_init` runs after the common per-user directories are available
and creates this version-1 document when the file is absent:

```text
config init -> immutable user-overrides snapshot -> resource-shadow install
            -> sparse installed-resource snapshot -> command-system install
            -> combined local/linked decl-server snapshot
            -> one private main-thread registration command
```

The snapshot makes the user-file layer stable for that DOOM process; the setting is changed for a later
launch, not as a live resource-loader switch.

## Override packages

An override package is one directory below `overrides/` holding its own content, marked by a `package.json`:

```
overrides/cyberdemon/package.json
overrides/cyberdemon/decls/<type>/<logical-name>.decl
overrides/cyberdemon/resources/<name>.manifest
overrides/cyberdemon/requirements/<name>.requirements
overrides/cyberdemon/shaders/generated/spirv/<name>.{vspv,fspv,cspv}
overrides/cyberdemon/shaders/generated/renderprogs/<name>_pc_vulkan.bin
```

Installing is copying the folder in; uninstalling is deleting it. There is no compile, staging or merge step:
DOOM never sees this layout, so the four consumers below each read N package roots instead of one shared root.
Nothing is duplicated on disk, no generated copy can go stale, and no package can leave artefacts behind after
its folder is gone.

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
shadow and is never searched. Packages are read in case-insensitive name order so two machines see the same
order, and an enumeration that could not complete -- unreadable subtree, over 64 packages, deeper than 8 --
refuses rather than running on a partial set.

### Packages compose; only disagreements are refused

Two packages overlapping is the normal case, not an error -- shared gore, FX and animation assets belong to no
single demon, and a package that vendors its own prerequisites is being self-contained, not wrong. So the same
rule applies at every layer:

| Layer | Two packages ship the same thing | They ship *different* things under one name |
|---|---|---|
| Decls | Byte-identical decls compose: the first copy serves the identity, the rest collapse (`decl-server COMPOSED`) | Refused, naming the packages that disagree |
| Resource manifests | Identical rows compose into one served entry | Refused, naming both provider rows |
| Requirements | The first request queues the command, the rest compose into it -- so `g_useResourceBlackList 0` asked for by three packages is issued once | A different value for an allowlisted name is refused |

Nothing overwrites anything and nothing wins by ordering. A package never has to know which other packages are
installed, and a genuine conflict fails closed with a diagnostic naming who conflicted rather than silently
handing the player the wrong asset.

Requirements are owned by the package, not by Snapmap+. Snapmap+ ships only a tiny allowlist of settings a
package is *permitted* to ask for; it sets nothing on its own. The cut-content blacklist cvars the Cyberdemon
needs live in `overrides/cyberdemon/requirements/cyberdemon.requirements`, so uninstalling that package removes
the request with it.

The pre-package layout -- a single shared `overrides/generated` tree -- is still read, reported as a package
named `generated`, so existing installs keep working unchanged.

### The file shadow resolves across packages too

The decl server publishes an identity, but the bytes the engine parses come from the file shadow, and the
engine only ever asks for a decl by its canonical virtual name -- `generated/decls/<type>/<name>.decl`. Before
packages that mapped one-to-one onto `overrides/generated/decls/...`, so joining the requested name onto the
overrides root *was* the resolver. A package owns its own root, so that join can never reach it: DOOM has no
idea `overrides/cyberdemon/` exists and will never ask for `cyberdemon/decls/...`.

So a `generated/decls/<rest>` request is resolved against the legacy tree first -- an install that predates
packages therefore resolves exactly where it always did -- and then against each installed package as
`<package root>/decls/<rest>`, in the same deterministic order. Only that one namespace is package-resolved,
and only a package's own `decls/` subdirectory, so a package can never expose its `package.json` as an engine
resource. The package set is captured once at install, because this sits on the engine's file-open path.

Without this the failure is silent and total: the identity registers, the engine opens nothing, and the parse
yields an empty default -- which for a `snapEditorEntityDef` means no resolved `entityDef`, a rejection by the
native palette validator, and a terminal materialization failure that refuses every decl behind it.

The same resolution serves a package's shaders. A render program is an ordinary decl type, so its
`decls/renderprog/<name>.decl` needs nothing new; but the compiled module is opened separately, by the engine
name `generated/spirv/<name>.{vspv,fspv,cspv}`, and its pre-translated source blob as
`generated/renderprogs/<name>_pc_vulkan.bin`. Both go through the same provider slot this layer hooks, with a
mode the hook admits. Under `shaders/` the package path mirrors the engine name verbatim, so a package can
carry a genuinely new render program and still uninstall by deleting its folder -- and two packages shipping
different programs can never address the same file, because the program name is part of the resource name.

Only the namespaces in that table are package-resolved, and each only out of the subdirectory named for it.
Everything else a package contains stays unreachable to the engine.

## Existing shadows versus genuinely new decls

The two mechanisms deliberately share one user setting and one data root, but solve different engine
problems:

| Path | Trigger | Result |
|---|---|---|
| Ordinary file shadow | DOOM requests an already-registered source path | The resource loader receives the user's bytes instead of the packaged bytes. |
| Installed resource bridge | Startup resolves `overrides/<package>/resources/*.manifest` against the installed base-game pindex; DOOM later requests an admitted virtual path | Snapmap+ reads and decodes that exact slice from the user's installed archive into an in-memory stream. |
| Dynamic decl server | Startup combines `overrides/<package>/decls/<type>/...*.decl` with linked game-owned decls; one main-thread command later excludes existing identities | The absent set is copied into an immutable exact `decltree/<type>/<logical-name>.decl` table, each source is submitted once to DOOM's native decl scanner, and missing `snapEditorEntityDef` objects are materialized before the derived palette rebuild. |
| Package requirements | Startup validates `overrides/<package>/requirements/*.requirements`; the backend tick waits for engine `load_state == RUNNING` | Product-audited, idempotent cvar requirements are queued once per process. Arbitrary console text is refused. |

The installed resource bridge is a sparse read-only source, not a virtual archive build. Each non-comment manifest
line is exactly three tab-separated fields: decl/resource type, logical name, and installed virtual path. An empty
third field uses the logical name as the provider path for the small number of pathless records. Snapmap+ parses the
user's installed `gameresources.pindex`. Multiple unique provider paths may share one type/name identity for
compiled resource bundles. Repeated exact triples collapse only when their selected metadata and stored payload
bytes are identical; divergent or over-cap repeats refuse the whole snapshot. Snapmap+ validates the selected archive,
offset, stored size, decoded size, provider-path uniqueness, and complete manifest bounds, then retains only the
selected metadata. It opens `gameresources.resources` and `gameresources.patch` read-only and decodes a selected
slice only when DOOM asks for that virtual path. The package contains no copied game payload bytes, and Snapmap+
never rewrites, stages, or replaces a pindex, archive, `.verify` file, executable, or game-side manifest.

Manifests are first-level regular files, capped at 64 files, 1 MiB each, 4 MiB total, and 4,096 exact resources.
Each decoded resource is capped at 64 MiB and the declared snapshot at 256 MiB. Duplicate provider paths,
unresolved rows, divergent exact-row repeats, traversal, non-ASCII fields, malformed pindex data, archive
bounds failures, compressed slices with no decoded bytes, incomplete/truncated/reserved DEFLATE
streams, nonzero alignment padding, trailing bytes after BFINAL or a sync-flush marker, concatenated streams,
or unexpected enumeration/read errors refuse the whole linked snapshot. A selected compressed slice must contain
one complete raw-DEFLATE stream ending in BFINAL, or the Doom archive's Z_SYNC_FLUSH form ending at an exact
non-final empty stored block (`LEN=0`, `NLEN=0xffff`); its pindex `zsize` is an exact boundary, not a
concatenated-stream container. Capture is a one-shot `NEW -> INSTALLING -> READY` publication: readers see no
snapshot while validation is in progress, and any refusal is terminal for that process. There is no partial
admission, fallback pindex, watcher, retry, or hot reload. On a matched resource read/decode failure, the hook
does not silently ask the engine for a different same-named row.

The dynamic path reuses the existing resource hook, but publishes a table of exact per-decl resources rather
than inventing an aggregate source. Discovery combines local generated decls with the linked `.decl` subset
after the resource bridge is proven ready. A same-identity local file deliberately wins over its linked
game-owned source, which lets a package patch selected dependencies without redistributing the originals.
Discovery, bounded structural validation, and dependency ordering happen on the backend bootstrap thread,
producing an immutable in-memory launch snapshot. Admission remains based on the complete deterministic
type/name/source ordering. Within that admitted set, a uniquely resolved logical name appearing as a quoted
value creates a dependency edge, so referenced decls are registered before their consumers. Comments,
escaped values, ambiguous names, and external identities do not create edges; independent entries remain
stable and cycle members retain their admission order. Snapmap+ registers one private engine command, waits
for `load_state == RUNNING`, and queues it through `BufferCommandText` after any admitted package requirements.
DOOM drains the ordered command buffer at its command-exec point on the main thread. That thread resolves each
short decl type through the registry's `+0x58` method and calls the clean signature-resolved `DeclSourceFind`
first. A non-null source record is classified `SHADOWED` without calling `DeclFind`; only when the source lookup
returns null does the command use `DeclFind(..., makeDefault=0)` as a live-object fallback. Existing source/live
identities are `SHADOWED` and excluded; unsupported types are `REFUSED`; absent identities are `MISSING`. An
exception from either native lookup is terminal and prevents table publication.

After classification, Snapmap+ copies every `MISSING` candidate's validated single brace body into an immutable
process-lifetime table keyed by the exact lower-case provider name `decltree/<type>/<logical-name>.decl`.
The provider checks this table before ordinary user, linked, built-in, and packaged layers; a matching entry is
authoritative and cannot be shadowed by a physical file. For each table entry, in dependency order, the handler
constructs a native 48-byte `idStr` containing `<type>/<logical-name>.decl`, invokes the registry's clean
signature-resolved `+0x38` `DeclRegisterFile(registry, &idstr, NULL)` once, and destroys the temporary with
`IdStrDtor`. The engine canonicalizes that source to `decltree/<type>/<logical-name>.decl`, opens the matching
body, derives the identity from the path, and scans one decl. Constructor, scanner, and destructor exceptions,
or a false scanner result, fail the one-shot service at the first candidate with no retry or fallback.

Only after every missing source scan succeeds does the command run its second phase. Registering a source
publishes the identity but does not by itself give it a live object, and DOOM's decl parsers resolve their own
`inherit`, `edit.entityDef` and game-ref edges with `makeDefault=0` — a lookup that succeeds only when the
target already has an object in its manager. The second phase therefore materializes per identity rather than
walking a typed dependency graph: every `MISSING` non-editor identity first, in the same dependency order the
scans used, then the eligible new `snapEditorEntityDef` roots. Each one is looked up with
`DeclFind(typeManager, logicalName, makeDefault=0)`, which lazily loads a pending object, and only a genuinely
absent object takes `makeDefault=1`. A null manager, a null object, an unreadable decl state byte at `+0x2c`,
an object still carrying the in-progress bit `0x01`, or a native exception is terminal for the one-shot
service. The generic valid bit `0x04` is recorded in the diagnostics but is not an admission condition,
because no engine consumer treats it as one.

An editor entity is held to one further contract, the same one DOOM's own palette validator applies: a
non-null resolved entityDef at `+0x1c8`, every output target flagged `0x20` and every input target flagged
`0x10` at `+0x3cd`. Before that check, the captured body must lexically carry a real top-level
`inherit = ...` or direct `edit.entityDef = ...` assignment; comments, quoted decoys, and nested fields do not
satisfy the gate, and source-only abstract bodies are classified `NON-PALETTE` and never materialized at all.
A refused root also emits a diagnostic probe of its typed edges so a cold run explains itself. The one-shot
palette builder is called once after every eligible root passes, and must return successfully before the
explicit registration-success bit is published. There is no aggregate alias, per-identity `AddFromText` call,
raw object cache, `DeclFind` detour, live rollback, or retry.

Registration reaches the engine's source catalog, and the engine stops consulting that catalog the moment
a map begins loading: `DeclFind` decides existence for an identity with no live object by branching on the
map-load lifecycle state, using the source catalog below state 2 and the decl-resource manager at state 2 and
above. Every gameplay map load is therefore blind to a freshly registered identity, which is why a placed new
entity used to report `Unknown entityDef` and never spawn. After the palette rebuild succeeds, the same
main-thread command arms `decl-visibility`, which answers that one existence probe for exactly the identities
in the published table and only after the engine's own answer was "no". It requires the manager's method slot
to already hold the pinned method for the supported build, forwards every argument of that method including
its trailing quiet flag, and corrects the boolean result alone -- the engine's output arguments are never
touched. The decl bytes are then read back through the file-system open slot the overrides layer already
serves. A refusal here is not fatal: registration still succeeds and new identities simply stay editor-only.

When both native phases succeed, the same main-thread registration command
invokes the one-shot `palette-refresh` operation synchronously. It validates
the editor singleton against the pinned module base and the palette object's
vtable against the pinned `module_base+0x20499A0`, then calls the clean
signature-resolved `SnapPaletteBuild(editor+0x20660, NULL)` exactly once for
either editor initialization state. Only a successful return publishes the
explicit registration-success bit; the backend never infers success from its
generic `DONE` state because that state also covers disabled, empty, and
all-shadowed snapshots. An unsupported signature, invalid object or vtable,
native exception, or false palette result is terminal `REFUSED`. There is no
tick poll or retry, rawmap hook, or literal `common.mapResources` injection.

## The override provider's pinned idFile ABI

The file-shadow returns a clean-room `idFile` stream with the exact 31 pointer slots used by the one
supported Steam build (`+0x00` through `+0xf0`). The table preserves the verified read/write/seek
methods, reports drive/storage slots `+0xc0=0`, `+0xc8=true`, `+0xd0=0`, and `+0xd8=0`, and refuses
`SetLength` at `+0x60` for both disk-backed and memory-backed streams. Read, write, read-at, and write-at
take the native 64-bit byte-count contract; memory streams remain bounded and read-only.

The final three slots (`+0xe0`, `+0xe8`, and `+0xf0`) use the engine's native ReadString, Compare, and
WriteString helpers directly. Snapmap+ does not reproduce their build-specific `idStr` ABI. The three
addresses are signature-resolved and all must be clean `SIG_OK` results before the stream table is
published; the table is fully configured before the resource-provider open slot is swapped, so an engine
thread cannot observe a partially populated tail. A missing, ambiguous, or hook-tolerant helper refuses
the provider for that process. At runtime Snapmap+ also requires the resolved ctor and three helper RVAs,
plus the decoded provider-vtable RVA, to equal the audited `DOOMx64vk.exe` Steam image. Signature matches on
an ABI-incompatible image therefore refuse before the hook is published. `DOOMx64vk_newbuild.exe` is not
supported until a new ABI/signature pass proves every slot. Memory cursor additions are overflow-checked;
an invalid or overflowing seek leaves the cursor unchanged.

Package requirements are deliberately narrower than console startup scripts. Each non-comment row is
`cvar<TAB>name<TAB>value`, and the pair must match a product-maintained allowlist. Identical requirements
from multiple packages compose; malformed, unsupported, reparse-backed, or unexpectedly unreadable input
refuses the complete requirements snapshot. The initial allowlist contains only the two cut-content
blacklist gates at value `0`. They are queued once after the engine reaches `RUNNING`, never during its
fragile startup decl parse, and there is no watcher, retry, hot reload, or arbitrary-command route.

The registry anchor, type lookup, source-register method, and decl finder are independently clean
signature-resolved. The anchor must be clean because Snapmap+ decodes its RIP-relative registry slot; live
vtable `+0x38` and `+0x58` must exactly match the resolved source-register and type-lookup addresses. Any missing,
ambiguous, hooked, or mismatched boundary refuses the service before table publication. A classification
exception refuses the table before publication; a scanner exception retains the already-exposed immutable
per-decl table and reports that DOOM may have partially cataloged the ordered prefix.

Discovery first captures the complete valid path-metadata set, up to a separate 4,096-entry safety ceiling.
The set is sorted with ASCII case-insensitive type/name/source ordering, every member of a case-insensitive
type/name collision group is refused, and only then are the first 512 non-colliding entries admitted. Collision
members do not consume the 512-entry quota, and a body-read refusal does not backfill its deterministic slot.
Each file is capped at 1 MiB and the admitted bodies at 16 MiB total. Reparse points, traversal, malformed paths,
embedded NULs, header punctuation outside the portable unquoted token alphabet, multiple top-level body blocks,
and unbalanced text are refused. A discovery allocation failure, safety-ceiling overflow, or
unexpected `GetFileAttributesA`, `FindFirstFileA`, or `FindNextFileA` result refuses the whole snapshot and queues
nothing; missing directories and normal end-of-enumeration remain non-errors. DOOM's parser remains the semantic
authority. The service intentionally has no watcher, refresh, retry, or unload path: changing a decl or resource
manifest requires a cold restart. The bridge can expose game-owned assets that already exist in the player's
installed DOOM archives; it does not embed dependencies in a SnapMap, download them from an author, or invent
arbitrary new models/textures/sounds that are absent from the installation. Disabling the user override layer or
removing the Snapmap+ DLL leaves the engine on its untouched packaged-resource path.

```json
{
  "schema_version": 1,
  "settings": {
    "theme": "light",
    "entities.show_hidden": false,
    "entities.selection_mode": "off",
    "overrides.user_enabled": true
  }
}
```

Deleting the file deliberately is therefore a clean reset: the next startup, or the next setting write
in a running session, recreates it. Missing or deleted configuration restores `overrides.user_enabled` to
enabled. Manual config edits are consumed at the next startup; a successful `sh_user_overrides 0` or
`sh_user_overrides 1` write goes through the existing setter and so recreates a deleted file. The one descriptor
table in `src/backend/config.c` declares each setting's key, JSON type, default, validator/normalizer, and
backend/frontend read/write permissions. In addition to `theme`, the registry has the
`entities.show_hidden` boolean, `entities.selection_mode` enum (`off`, `follow`, or `select_in_3d`), and
`overrides.user_enabled` boolean (true by default); the schema version and generic backend↔frontend ABI
are unchanged. Adding a setting means adding a descriptor and its behavior/tests; the wire contract remains
generic.

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
