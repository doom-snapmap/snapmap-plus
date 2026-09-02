# Backend changes — engine-call bugfix log

A running log of correctness bugs found and fixed in `src/backend/` — the shared engine-call layer
behind the frontend. These are cases
where our own reimplementation was wrong, not the original SnapHak's behavior; a divergence from
(or faithful reproduction of) the *original's* behavior belongs in [`fidelity.md`](fidelity.md)
instead. Entries are chronological, newest first.

## 2026-09-01 — Stop the rawmap save shadow from reporting the player's save as failed

**What changed.** `sh_ser_detour` in `rawmap.c` is typed `unsigned char` instead of
`void`. It latches the engine's return value the instant `SerializeToJson` returns
and gives that same value back on all seven of its exit paths. Nothing the shadow
does afterwards — the arm check, the idStr read, the pretty pass, the disk write —
can speak for the engine any more.

**Why.** `idSnapMap::SerializeToJson` returns a bool in `AL`, and that bool is the
save's success flag. Its sole caller, the save-snapshot function at RVA `0x59D2F0`,
does `MOVZX EBX,AL` on the instruction after the `call` and later returns it as its
own result (`MOVZX EAX,BL`) — read directly out of the pinned build at `0x59D2F0+0x54`.

The detour was typed `void`, so whatever it happened to call last decided the save's
fate. With the shadow disarmed — the default — the last call before returning was
`GetFileAttributesA`, probing for the `arm.flag` file inside `rawmap_armed()` ->
`flag_file_present()`. That file is normally absent, so the predicate returned 0, and
the engine read `AL` as a serialize failure. The save was abandoned with no error, no
log line and no file written: the editor kept the map dirty, re-prompted for a map
name on exit, and the map never appeared in My Maps. With the shadow armed the
trailing shadow work left `AL` non-zero, which is why `sh_rawmaps_on` appeared to fix
saving — luck, not design.

The regression arrived with the arm gate (the `if (!rawmap_armed(NULL)) return;`
added when the SAVE shadow was put behind the same switch as the LOAD swap). Before
that gate there was no early return, so the ungated path always left `AL` non-zero by
the same accident. The load-side detour never had the defect: it is typed `int`,
latches `rc`, and runs its arm check *before* calling the engine rather than after.

**The invariant this exposed.** A detour that does any work *after* calling the
original must declare the original's real return type, latch the value, and return
it. A detour that calls the original last is safe for free, because the original's
return passes straight through. Two detours still have the risky shape and are safe
only because their targets' returns are unused — `sh_sort_detour` in `strids.c`
(target `0x1A2B490`; its caller's next instruction is `MOV ECX,[RSP+0x20]`) and
`ds_boot_promotion_detour` in `decl_server.c` (target `0x1801830`; its sole caller at
`0x17C6479` continues with `MOV RCX,[rip+...]`). Both now carry a comment recording
that dependency, so the next person does not have to rediscover it the hard way.

**Status.** Confirmed in the live editor on the pinned build: with `sh_rawmaps_off`
a map saves, exits without re-prompting, and appears in My Maps. Saving with
`sh_rawmaps_on` still mirrors `rawmap.json` exactly as before.

## 2026-08-23 — Let a package own its shaders, not just its decls

**What changed.** The cross-package resolver is now a namespace table rather than
one hardcoded prefix. `generated/decls/` still maps to a package's `decls`
subdirectory as before; `generated/spirv/` and `generated/renderprogs/` now map
into a new `shaders` subdirectory, where the package path mirrors the engine
resource name verbatim. Only the enumerated namespaces are package-resolved and
each only out of its named subdirectory, so nothing else a package contains
becomes reachable.

**Why.** A render program is an ordinary decl type, so its
`decls/renderprog/<name>.decl` already rode the existing path -- but the compiled
module is opened separately, by the engine name
`generated/spirv/<name>.{vspv,fspv,cspv}`, through the same provider vtable slot
this layer hooks (the call site at RVA 0xD923A3 passes mode 0, which the open
hook admits). Without a package-scoped route those files would have to sit in the
shared tree, where two packages could overwrite each other's shaders on disk --
the exact failure packages exist to prevent -- and deleting a package folder would
no longer fully uninstall it.

**Status.** The resolution path is covered by `override_packages_test`. Nothing
has yet bound a custom shader in a running game; the remaining unknown is whether
a renderprog decl registered at load-state RUNNING reaches the bind cleanly.

## 2026-08-23 — Resolve a shadowed decl out of any installed package, not just the shared tree

**What changed.** `overrides.c` now resolves an engine resource name across every
installed override package instead of joining it straight onto the overrides
root. A `generated/decls/<rest>` request is tried against the legacy shared tree
first, then against each package as `<package root>\decls\<rest>` in
`sh_packages_enumerate` order; the first existing file serves it. Only that one
namespace is package-resolved, and only a package's own `decls` subdirectory, so
a package cannot expose its `package.json` or anything else as an engine
resource. The package set is captured once at install, alongside the existing
audit and reclaim passes, because this sits on the engine's file-open path.

**Why.** The per-package migration taught the decl server, the resource bridge
and the package requirements reader to read N roots, but not the file shadow --
and the file shadow is what actually hands decl bytes to the parser. The engine
only ever asks for a decl by its canonical virtual name,
`generated/decls/<type>/<name>.decl`, which reached `overrides\generated\decls\...`
and therefore only ever matched a package literally named `generated`. Every
other package's decls were unreachable. The failure was silent: the identity
registered, the engine opened nothing, and the parse produced an empty default.
For `snapeditorentitydef/demons/cyberdemon_enc` that meant no resolved
`entityDef`, rejection by the native palette validator, and a terminal
materialization failure that refused the 292 decls queued behind it -- so the
Cyberdemon never appeared in the Toybox. A package's own patched copy of an
identity DOOM also ships was likewise ignored in favour of the archive original.

**Proof.** Live, on the pinned build: `file-shadow FIRED [user]` went from 50
fires (all under `generated/`) to 66 including 11 from the `cyberdemon` package;
`entitydef/ai/demon/cyberdemon_hell` now serves the package's 3023-byte patched
copy instead of the archive's 2869-byte original; and the decl server went from
`293 REFUSED; materialization was terminal` to `289 live objects materialized,
0 REFUSED` with `palette-refresh FIRED`. Covered by `override_packages_test`,
which fails on the package cases when the cross-package search is disabled while
the legacy-tree case still passes.

## 2026-08-21 — Keep published decl identities resolvable across a gameplay map load

**What changed.** A new service, `decl_visibility`, answers DOOM's decl-resource
existence probe for exactly the identities the dynamic decl server published,
and only after the engine's own answer was "does not exist". It resolves the
decl-resource manager through the pinned global at RVA `0x5557090`, requires the
vtable `+0x78` slot to already hold the pinned method at RVA `0x1806100`, and
refuses otherwise. The hook forwards all **seven** arguments of that method,
including the trailing quiet flag. It claims a path only when the path sits
under `generated/decls/` and names an entry in the immutable published table,
and it corrects the boolean result alone, leaving every output argument exactly
as the engine wrote it. `decl_visibility` is armed once, on the same main-thread
command, after the palette rebuild succeeds; a refusal is never fatal to
registration and only means new identities stay editor-only.

**Why.** `DeclFind` decides existence for an identity with no live object by
branching on the map-load lifecycle state at RVA `0x6DDE198`. Below state 2 it
consults the source catalog that `DeclRegisterFile` populates; at state 2 and
above -- which covers every gameplay map load -- it asks the decl-resource
manager instead, and nothing this product wrote reached that manager. So a
gameplay load logged `Unknown entityDef` for identities that had registered and
materialized cleanly minutes earlier, and no actor ever spawned. With the hook
installed, a cold run registered 232 identities with zero refusals, the engine
resolved the whole actor dependency tree during the Play load, and the demon
spawned, animated, engaged and killed the player.

The seven-argument detail is load-bearing and was learned the hard way. Argument
7 is a quiet flag: a cache miss returns false silently when it is non-zero and
raises a fatal engine error naming the path when it is zero. An earlier
six-argument forward left that slot as stack garbage, which turned ordinary
silent misses into fatal errors and aborted a fully loaded map back to the
SnapMap browser.

**Also.** A placed editor entity from an override package rendered with a zero
scale, so its model matrix was singular: the engine logged `modelMatrix invert
failed` every frame, forced the axis back to identity, and the entity could not
be transformed. The scale a Snap editor entity uses comes from its entityDef's
`renderModelInfo.scale`, which a shipped entityDef inherits as a unit vector and
a package-published one did not. Declaring it explicitly in the package's
entityDef fixes the preview; no product code was involved.

## 2026-08-20 — Use DOOM's own palette contract to admit new editor entities

**What changed.** The dynamic decl server no longer walks a typed dependency
closure and no longer requires the generic decl valid bit. Materialization is
now per identity: every `MISSING` non-editor identity is given a live object in
its own manager first, in the dependency order the source scans used, and the
eligible new `snapEditorEntityDef` roots follow. Each identity is looked up with
`DeclFind(..., makeDefault=0)` and only takes `makeDefault=1` when no object
exists at all. A null manager, null object, unreadable `+0x2c`, in-progress
object, or native exception remains terminal. An editor entity is additionally
held to the same contract DOOM's palette validator applies: a non-null resolved
entityDef at `+0x1c8`, every output target flagged `0x20`, and every input
target flagged `0x10` at `+0x3cd`, with the target-array counts bounded. Shadowed
identities are still never synthesized, source-only abstract bodies are still
`NON-PALETTE`, and the palette builder is still called exactly once. New
`DECLSTATE` diagnostics record each observed object's pointer, state byte,
resolved entityDef, and target counts, and a refused root additionally probes
its typed edges.

**Why.** Static reverse engineering of the pinned build showed three things.
The palette builder enumerates every live editor entity and admits exactly what
its validator accepts; that validator never reads `+0x2c`, so requiring the
valid bit was stricter than the engine itself and made an ordinary installed
parent terminal. A manager lookup already lazily loads a pending object, so the
valid bit records only that a load acquired a source. And the decl parsers
resolve their own inherit and entityDef edges with `makeDefault=0`, which
succeeds whenever the target has an object — so the generic requirement is that
every registered identity has an object, not that the loader pre-resolves a
typed graph. Dropping the closure also removes the per-decl-type edge schema a
broader package format would otherwise have needed. Tests cover the palette
contract directly, including in-progress objects, unresolved entityDefs,
mis-flagged input and output targets, materialization order, and the rule that a
shadowed identity is never synthesized.

## 2026-08-20 — Classify declaration sources before live objects

**What changed.** The dynamic decl server now resolves the native
`DeclSourceFind(typeManager, logicalName)` routine before using
`DeclFind(..., makeDefault=0)`. An existing source record is classified as
`SHADOWED` and never enters the missing table, scanner, materialization pass, or
palette refresh. Only a null source record falls back to the live-object lookup;
both-null identities remain `MISSING`, and a null type manager is `REFUSED`.
Source and live lookup exceptions are terminal and prevent table publication.
The new direct signature is pinned to RVA `0x17B34B0` and the install gate
requires a clean resolve at that exact address.

**Why.** Native source registration can leave an existing source record
unmaterialized, so object-only classification incorrectly treated it as absent
and attempted to register it again. Source-first classification matches the
engine's scanner semantics while retaining the live-object fallback for records
that were created independently. Focused tests cover source-first call order,
all classification outcomes, terminal lookup faults, and the clean pinned
install gate.

## 2026-08-20 — Materialize new editor-entity decls before palette rebuild

**What changed.** The dynamic decl server now has an explicit second phase
after every missing source scan succeeds. Before any native make-default call,
it lexically checks each captured `snapEditorEntityDef` body for a real
top-level `inherit = ...` or direct `edit.entityDef = ...` assignment. Comments,
quoted decoys, and nested fields do not satisfy this gate; a source-only
abstract body is classified `NON-PALETTE` and never calls `DeclFind(...,
makeDefault=1)`. Eligible identities then take the pinned native
`DeclFind(typeManager, name, makeDefault=1)` path and require a readable decl
state byte at `+0x2c` with the in-progress bit clear and the valid bit set plus
a non-null resolved entityDef pointer at `+0x1c8`. A null manager, null object,
fault, unreadable state, in-progress object, or invalid object is terminal and
prevents both registration success and palette refresh. A valid abstract/mispath
object with a null entityDef is logged as `NON-PALETTE`, excluded from palette
admission, and does not abort unrelated valid sedefs. The
palette service now returns success/failure, no longer treats editor `+0x08 ==
0` as a satisfied branch, validates the palette object, and calls
`SnapPaletteBuild(editor+0x20660, NULL)` exactly once for either initialization
state. The registration-success bit is published only after that return is
successful; a false result is terminal.

**Why.** Native source registration creates the source record but does not
guarantee a live decl object for a new identity. The editor palette builder
enumerates live `snapEditorEntityDef` objects, so it cannot discover a new
entity until the native make-default lookup materializes it. Source-only
abstract records are valid catalog entries but have no direct entityDef or
inherit assignment and must not enter that native lookup. Abstract records that
do materialize are still valid catalog entries but have no resolved entityDef
and cannot be palette entries; the non-palette classifications keep them from
aborting valid neighbors. The two-phase
sequence keeps all source scans ahead of materialization, remains generic to
the decl table, and changes no rawmap, archive, or `common.mapResources`
state. Focused tests cover the comment/string-safe lexical gate, zero native
calls for source-only bodies, scan-before-materialization wiring, the
make-default argument, type filtering, state validation, terminal faults, and
exactly-once palette calls for both initialization states.

## 2026-08-20 — Rebuild the editor palette after new decl registration

**What changed.** The dynamic decl server now exposes a success result only
after every missing identity has completed the native source scan and the
one-shot `SnapPaletteBuild` call has returned successfully. The ordinary
engine tick does not poll or retry it. The backend validates the editor
singleton against the pinned module base and the palette object's vtable
(`module_base+0x20499A0`) before invoking the clean-signature routine with
`editor+0x20660` and a NULL progress argument. Unsupported signatures, invalid
objects/vtables, false results, or exceptions are terminal and cannot retry or
call twice.

**Why.** A decl can be fully present in the native catalog while the already
built SnapMap editor palette still lacks its entity entry. Rebuilding the
engine-owned derived list closes that gap without a rawmap hook, a literal
`common.mapResources` edit, or a persistent game-file change. Focused tests
cover the refused/applied states and exactly-once consumption; the real image
signature test pins `SnapPaletteBuild` at RVA `0x54AEE0`.

## 2026-08-20 — Require complete bounded DEFLATE slices

**What changed.** The raw-DEFLATE reader now fails closed on every malformed, truncated, reserved-block,
non-final, or output-overflow condition instead of returning partial bytes. It no longer treats reaching the
requested output size as success: a complete BFINAL block and its EOB are required, including when an empty
final block follows a full non-final block. Dynamic literal/length trees must explicitly contain symbol 256
(EOB).

The pindex `zsize` is treated as an exact compressed slice. After BFINAL only zero alignment bits in the last
byte are accepted; concatenated streams and trailing bytes are refused. The one narrow non-final exception matches
Doom's archive compressor: a slice may end immediately after a non-final empty stored block with `LEN=0` and
`NLEN=0xffff`, the raw-DEFLATE representation emitted by `Z_SYNC_FLUSH`; its alignment padding must also be zero.
Focused tests cover fixed and dynamic known-good streams, missing EOB, exact-size output followed by truncation,
non-final EOF, valid/invalid sync-flush termination, reserved blocks, oversize output, legal empty-final-block
termination, and trailing-byte refusal.

## 2026-08-20 — Complete the pinned 31-slot idFile provider table

**What changed.** The file-shadow stream now exposes the exact 31-entry idFile vtable used by the
supported Steam build, including the verified storage slots at `+0xc0..+0xd8` and a read-only
`SetLength` refusal at `+0x60`. Read/write byte counts use the native 64-bit contract. The three
build-specific idStr helpers at `+0xe0/+0xe8/+0xf0` are installed as native engine addresses only when
all three signatures resolve cleanly; helper publication completes before the provider open-slot swap,
and a dirty or missing helper fails closed without exposing a partial table. The product remains pinned
to the original `DOOMx64vk.exe` Steam image and does not claim support for `DOOMx64vk_newbuild.exe`.
The install now enforces that boundary: the provider ctor, all three native helper addresses, and the
decoded provider vtable must occupy the audited RVAs before the hook is published. Signed cursor additions
are overflow-checked, negative file lengths are refused, and an explicit installed-package probe fails when
it finds no manifest-backed entries instead of reporting a vacuous pass.

Focused contract tests cover the 31-slot shape, helper all-or-nothing publication, terminal refusal,
native helper RVAs, SetLength behavior for both memory and file-backed streams, supported-build admission,
and overflowing-seek cursor preservation. The real-image signature test resolves all three helpers uniquely
at `0x267390`, `0x267290`, and `0x268470`.

## 2026-08-20 — Reject malformed resource streams before publication

**What changed.** The bounded raw-DEFLATE decoder now validates each canonical Huffman code space while
building code-length, literal/length, and distance trees. Oversubscribed trees and incomplete trees outside
the DEFLATE/zlib exceptions are rejected; the one-symbol one-bit EOB-only literal tree, all-literal
zero-distance tree, one-bit single-distance tree, and predefined fixed-distance tree retain their defined
behavior. The resource bridge also refuses a
selected slice that declares compressed bytes but zero decoded bytes, rather than treating a zero-byte decoder
result as a successful payload.

The bridge now claims its one-shot capture with a single `NEW -> INSTALLING` compare-and-swap, publishes
`READY` only after the complete manifest/pindex/archive snapshot is validated, and enters terminal `FAILED`
on any refusal. Reader APIs return no snapshot data while capture is installing, so no caller can observe
partially built globals. Focused tests cover oversubscribed/incomplete trees, every legal exceptional tree,
the zero-decoded compressed slice, and the read-only stream's native true/no-op slots.

## 2026-08-20 — Match the native idFile seek/read/write helpers

**What changed.** The override provider now uses the engine's idFile seek-origin contract: `0` is
relative-to-current, `1` is relative-to-end, and `2` is absolute; any other origin refuses without
moving the cursor. The `+0x38` helper performs an absolute seek followed by read, and the `+0x40`
helper performs the independently verified absolute seek followed by write. Memory-backed provider
streams remain read-only, including the write-at path. Focused native tests cover all three origins,
invalid-origin preservation, absolute read-at, and read-only write-at behavior.

## 2026-08-20 — New decls enter the native source catalog

**What changed.** The dynamic decl server no longer exposes raw `AddFromText` objects through a DeclFind
detour, and it no longer frames multiple identities into an aggregate source. Those objects can be registered
yet lack the canonical parsed state consumed by inheritance, which made a nested parent lookup return an object
that immediately faulted during entityDef field copying. The server now classifies all existing identities with
lookup-only `DeclFind(..., makeDefault=0)`, excludes them as ordinary `SHADOWED` file overrides, and copies only
the absent set into an immutable exact table of `decltree/<type>/<logical-name>.decl` entries. Each table body is
one validated brace block; the main-thread command invokes the registry's native `+0x38` source scanner once
per identity in dependency order, letting DOOM create its own catalog records through the source-file
architecture it expects.

The path is fail-closed: registry `+0x38` and `+0x58` must match clean pinned signatures, each identity and
body is bounded and structurally validated, and publication happens only after complete classification.
The source-register method takes a native 48-byte `idStr` by pointer and returns a boolean-like result. The
server constructs a temporary containing `<type>/<logical-name>.decl` with the resolved `IdStrCtor`, calls the
scanner exactly once for that candidate, and always destroys it with `IdStrDtor`; a constructor/scanner/
destructor exception or false scanner result terminates the ordered sequence at that candidate with no alternate
C-string call, fallback, or retry. The provider serves only published exact table keys before ordinary layers;
it never exposes an aggregate alias. There is no raw object cache, post-scan lookup, default synthesis, hot
reload, or DeclFind code patch. Native tests cover single-body validation, immutable exact-key publication,
case-sensitive provider matching, deterministic dependency ordering, and the constructor -> scan -> destructor
boundary including terminal failures.

## 2026-08-17 — Cut-content packages no longer need rebuilt game archives

**What changed.** The override provider now accepts bounded metadata-only manifests under
`overrides/<package>/resources`. At launch it resolves every exact manifest triple against the user's installed
base-game pindex, validates the complete sparse set and archive bounds, and keeps the source archives read-only.
When the engine asks for an admitted virtual path, Snapmap+ reads and raw-DEFLATE-decodes just that slice into its
existing in-memory `idFile` stream. The dynamic decl server consumes linked game-owned `.decl` rows from the same
snapshot; a local generated decl of the same identity wins so packages can carry only their authored patches.
One logical resource may enumerate multiple unique virtual files, as render-program and other compiled bundles
do. Exact pindex duplicates collapse only after byte-for-byte stored-payload equivalence; divergent repeats fail
the complete launch snapshot.

**The prior limitation.** Loose file shadows could replace a path only after the SnapMap resource set already knew
about it, while native decl registration could add text identities but not their referenced models, animations,
images, collision, behaviors, and other campaign-only dependencies. The historical Cyberdemon proof rebuilt a
large SnapMap patch/pindex and regenerated sidecars. The sparse bridge supplies the same installed bytes on demand
without copying or changing a game archive, pindex, sidecar, executable, or mapResources ID table. Admitted new
decls are then ordered by unambiguous quoted logical-name references, which lets foundational particles, sounds,
and other supplied decls register before the higher-level decls that consume them without package-specific names
or encounter rules. Removing the
Snapmap+ DLL therefore restores the untouched engine resource path.

The native test uses a synthetic pindex/archive to prove exact resolution, compressed decode, provider gating,
path-tolerant lookup, multi-file identities, equivalent-repeat collapse, divergent-repeat refusal, provider-path
collision refusal, and truncated-stream rejection. A separate installed-data probe decodes the Cyberdemon
dependencies from the user's own archives without emitting a resource payload.

## 2026-08-17 — Decl discovery could split collisions at its capacity boundary

**What changed.** The dynamic decl server now enumerates a bounded metadata snapshot before admitting any file.
It sorts that complete set deterministically, refuses every member of each case-insensitive type/name collision,
then admits the first 512 non-colliding entries. Unexpected root, first-entry, or next-entry filesystem errors
abort and discard the whole snapshot before a main-thread command can be queued.

**The bugs.** The 512-entry cap previously ran while the operating system was still enumerating directories.
If two case variants of one logical identity straddled that boundary, the retained member appeared unique and
could be registered while its twin was merely over-cap. `FindFirstFileA` and `FindNextFileA` failures were also
treated as an empty directory or normal end, so a partial traversal could be applied as though it were complete.

The native regression test places a collision on opposite sides of a 515-entry input, proves that both members
are excluded before the 512-entry admission boundary, and checks the deterministic last admitted identity. A
scripted Win32 enumeration seam proves normal empty/end behavior, root and mid-stream access failures, recursive
failure propagation, whole-discovery discard, and exactly-once handle closure. The source-wiring contract also
pins metadata walk, collision analysis, and capacity admission in that order.

## 2026-08-15 — Asset catalogs retained complete source tables after parsing

**What changed.** The installed resource indexes are now reduced to interned recognized-name and
payload-offset metadata after their first use, releasing the complete raw index buffers. The broader
base-game index keeps only material, image and sound records -- the routes that can actually serve
this browser. The Wwise manifest is streamed only when Sounds or its soundbank qualifier is
requested, retaining only event/bank tags transiently and one copy of each event and bank name. The
decl-less `.vmtr` union is likewise deferred until Materials is requested.

**The cost.** Opening any category previously loaded every optional catalog on the native side even
after the UI stopped asking for them. It also retained the complete resource indexes and the complete
Wwise XML document because catalog strings pointed into those buffers. That made an Images or Models
request pay for sound and material-only metadata and kept tens of megabytes of source data alive.

Against the installed files, the parser keeps 67,338 useful records rather than all 95,568 recognized
records and reduces 30,173,667 raw index bytes to 2,855,718 interned name bytes. When Sounds is
requested, 26,345,897 Wwise XML bytes stream through 2,026,023 relevant tag bytes and become 240,251
retained string bytes. Together that is 53,423,595 fewer retained source bytes (50.9 MiB) after the
sound catalog has been visited, before the additional record-array reduction.

The Wwise event and bank-row pointer tables are now shrunk after deduplication too. On the installed
manifest they retain 134,488 bytes for 1,513 event-only names and 7,649 bank rows instead of keeping
1,179,648 bytes of parser growth capacity, removing another 1,045,160 bytes. The decl-less-material
pointer table likewise shrinks from 16,384 to 14,144 bytes for its final 1,768 rows.

Mega2 previously copied a touched shard's complete page index and offset table into memory. Across
the 16 installed shards those tables total 71,224,640 bytes (67.9 MiB). Preview lookup now seeks to
one 4-byte page id and one 16-byte offset/size entry, validates them against the file, then reads only
the selected page payload. VMTR rect storage also falls from a fixed 1,703,936-byte row array to
509,480 bytes of exact rows and names on the installed tables. Decode scratch is allocated only for
an atlas-backed request and released after 30 seconds idle.

Published PNG data is now consume-on-read. Changing type, selection, tab, or modal cancels the
generation, releases an unconsumed backend buffer, and removes the WebView data URI; late replies are
ignored by name. This prevents a finished or abandoned preview from remaining as duplicate encoded
asset content.

The native tests prove an Images request leaves both optional loaders untouched, the matching public
list requests activate them, raw indexes can be released without invalidating interned names, every
Wwise event/bank pointer lands in the compact owned pool, Mega2 reads selected entries without full
table arrays, preview buffers are consumed or cancelled, and UI catalogs remain within two cache
slots.

## 2026-08-15 — Image rows were rejected or captured by a same-named Material

**What changed.** The WebView now sends the selected asset kind with each preview request. The
matched host carries an Image kind through the existing append-stable request slot, and the worker
sends it straight to an Image-only producer before loading or searching VMTR. Materials keep their
existing atlas-first and ordinary-image fallback. Both use the same bounded decode, downscale and
generation-safe publish path.

**The bug.** The Images category showed the same preview affordance as Materials, but selecting any
image originally failed with "no material record." Allowing a direct-image fallback fixed that for
most names but left an ambiguity: installed data contains names represented by both a Material and
an Image record. Because the request carried only a name and material lookup ran first, an Images
row with one of those names could preview the Material's mapped image instead of the selected Image.

**Why.** The producer was written for material selections: its first operation was a
`SH_ASSET_MATERIAL` lookup. Adding the Images category exposed both the missing direct-image case
and the fact that a name alone cannot disambiguate overlapping catalog namespaces.

The regression test builds a minimal 4x4 BC1 `.bimage` plus an unusable same-named Material in a
temporary file and proves the typed Image path publishes the expected RGBA pixels. A worker test
also proves that route does not initialize VMTR metadata or atlas scratch.

## 2026-08-10 — the asset catalog listed the same asset twice, because the game index is not a catalog

**What changed.** `imgpreview.c` now collapses records that repeat a name within one resource box,
before anything else reads the record list.

**The bug.** Decal atlases appeared twice per asset in the browser. Clicking one row selected both and
starring one starred both — which looked like a selection bug and was not: the UI keys off the asset
name, and the catalog genuinely held two rows carrying the same one.

**Why.** We had been treating `snap_gameresources.index` as a catalog of distinct assets. It is a
record-per-blob table: the same decl can be baked into the `.resources` file more than once, at
different offsets, and the index lists each copy. Measured across the box — `decalatlas` 1,673 records
for 1,024 distinct names, `image` 3,423 for 3,422, and exactly zero repeats for `material`, `model`,
`md6Def`, `sound`, `fx`, `particle`, `entityDef`, `snapEditorEntityDef` and `cm`. So the collapse is
written generally but only ever fires where the data actually repeats.

The first record wins, which is what `find_rec` would have resolved to anyway, so nothing that already
previewed changes which blob it reads.

**Adjacent, unresolved.** The sound catalog files 8,028 rows against 7,649 distinct Wwise events with
378 unbanked, which leaves one more row carrying a bank than there are events to carry. Sound names are
unique in both boxes, so the obvious explanation — two names differing only by case — is ruled out. One
row in 8,028, no known consequence, recorded here rather than chased.

## 2026-08-05 — a sound preview could take the whole sound engine down: the emitter list is published before it is initialised

**What changed.** Every audition now runs on the main thread instead of the caller's.

**The bug.** Previewing a sound could fault and then freeze the game (owner-reported: an access
violation followed by a hang).

**Why.** The race is in the engine, not in our code — but we were the ones exercising it. DOOM's
`StartSound_wwise` publishes a brand-new emitter into the sound world's **live list** before
initialising it: the emitter is allocated and base-constructed, written into the world's array and the
count bumped, and only *then* is its sound-shader pointer set. `idSoundWorld::Update` walks that same
list on an audio worker thread, and `idSound::Update` opens by dereferencing exactly the pointer that
is still null inside that window.

Calling the preview from any thread but the main one widened a window the engine gets away with
internally. Serialising our calls onto the main thread closes it from our side without patching engine
code.

## 2026-08-05 — the SWF text editor latched Ctrl: a bare `c` copied and a bare `v` pasted

**What changed.** `swf_textedit.c` no longer tracks Ctrl in a static flag consulted on later
keystrokes.

**The bug.** After one Ctrl+C or Ctrl+V, every subsequent bare `c` or `v` typed into any editor text
field copied or pasted — typing "variable" pasted the clipboard once per `v`. Owner-reported, confirmed
fixed live.

**Why, and the lesson.** The flag was set on Ctrl key-down and cleared on key-up. Modifier key-ups are
not reliably delivered to a focused SWF script object — the field or the window can lose focus, or the
engine may simply not dispatch them — and one missed key-up left the flag stuck on for the session.

The design had been justified in a comment as tracking Ctrl "exactly the way the stock handler tracks
Shift", and that is precisely where the reasoning failed: the stock handler reads Shift only while
processing the keystroke it was handed, so a release it never sees costs nothing. Ours was consulted on
*later* keystrokes, which turns the same pattern into a permanent latch. The mechanism was copied
without its lifetime assumption.

## 2026-08-05 — the last hardcoded engine RVAs are gone, and the `[64]` that hid a truncated signature DB bit again

**What changed.** Three engine functions were still reached as a raw `module_base + RVA`, which is
build-locked in exactly the way the signature resolver exists to avoid. All three are now ordinary
signature entries, verified unique across the whole executable image:

| Signature | Pinned RVA | What it is |
|---|---|---|
| `Mega2PageDecode` | `0x196E140` | the megatexture page decoder the asset browser calls |
| `PrefabDtor` | `0x51D870` | `idSnapEntityPrefab::~idSnapEntityPrefab` |
| `EntityDeshare` | `0x52C920` | COW make-unique, before an in-place entity edit |

The two prefab functions join the ctor/populate/`MemLocal` group on `ae_pick_engine_fn` — signature
wins, `known_rva` cross-checks and logs a mismatch loudly rather than silently preferring either.

The decoder previously carried a **local `memcmp` of its own prologue** at the hardcoded address.
That is strictly weaker than a signature and the difference is not academic: a byte check can detect
that a function moved but can never *find* it, so any build with a shifted `.text` lost previews
outright. It now resolves wherever the loader put it, and a build whose bytes genuinely differ fails
to resolve — which still disables previews, but by refusing rather than by calling into whatever now
occupies that address.

**One function could not be signatured, and is not pretending to be.** idList-grow (pinned
`0x699A60`) is **one of 1,560 byte-identical instantiations** of the same idList template in the
image, separable only by their displacements. No lengthening of a prologue pattern gets past that —
the same wall `StopSound`'s clone hit. It is resolved *relationally* instead: `AddCommand` (which is
signatured) calls it on `cmdSys+0x08` via `LEA RCX,[RSI+8]` / `CALL rel32`, so the callee is decoded
out of that call site, range-checked against the module, and logged with the pinned RVA as a
cross-check. The decode runs before our own `AddCommand` detour is installed, and the call site sits
well past the 15 stolen prologue bytes either way.

**The bug this uncovered.** `tests/sig_test.c` and `tests/hooktol_test.c` both still declared
`sig_result results[64]` — the same fixed array that silently truncated the database at entry 65 and
prompted `SIG_RESULTS_MAX` in the first place. The constant was applied to the shipping callers and
missed in the tests. At 67 entries it stopped being silent and became a stack overwrite:
`sig_resolve_all` wrote past the end of the array, producing garbage status values and an access
violation **inside the test process**. Both now size with `SIG_RESULTS_MAX` and print a loud
`SIGNATURE DB OVERFLOW` and fail if the database ever outgrows it.

The general lesson is worth keeping: a constant introduced to fix a class of bug only fixes the call
sites it is actually applied to. Grep for the literal, not just for the symptom.

**Verified.** `sig_test` 67/67 signatures resolve to their known RVAs; `hooktol_test` 3/3
hook-tolerant fallbacks; 16/16 native tests; `go test ./...` clean.

## 2026-07-28 — Load/Place now picks the prefab up (auto-grab enabled): the corruption was the heap bug

**What changed.** `kind=2` (stage → pick up) is enabled. Load/Place no longer asks the user to press Ctrl+V.

**Why it was off.** An unexplained `"Memory corruption before block!"` had followed two auto-grab pastes,
while repeated manual pastes looked clean, so auto-grab was implemented and left disabled. Two suspects were
on record, both states the engine never produces for itself: we **set** the paste-available bit rather than
letting the engine recompute it, and we **ClearSelection** immediately before arming.

**Why it is on now.** Re-tested after the map-heap fix below, with instrumentation:

- **Eight auto-grab pastes across two sessions** — including three on a fresh map followed by several
  minutes of ordinary editing — produced **no corruption**.
- **Post-paste editor state is identical to a manual Ctrl+V:**
  `mode+0x1ac=4  arm(+0x420)=-1  action=0x0  flags1(+0x41)=0x64 pasteAvail=1  flags2=0x00 dirty=0`.
  So neither suspect leaves any residue, and the arm word self-clears — an injected action cannot re-fire.
- The original corruption is therefore attributed to the **staged-prefab map-heap bug** fixed the same day
  (entry below), which was live at the time of every earlier auto-grab observation. Consistent with the fact
  that the "auto-grab specifically" reading rested on a manual control whose two recorded accounts
  contradicted each other.

**We do not bypass a capacity check.** Read from the engine's gate recompute, the paste-available bit is
*exactly* two conditions — the copy/paste cvar AND staged entity count ≥ 1 — inside the nothing-hovered
branch. **No budget term.** And because this route injects an action rather than calling `PasteInstantiate`
directly, the engine still runs its own paste branch and every check inside it. Separately confirmed: the
in-editor budget rows are **soft** (the editor deliberately allows exceeding them), so they are not something
to gate on either.

**The one honest residue:** we set the bit without reading `snapEdit_enableCopyPaste`, so a paste is possible
while the user has copy/paste disabled. Minor — the engine's next recompute clears the bit again.

**Degradation is unchanged and still load-bearing.** Any of "not in EntityMode", "already holding"
(`mode+0x1ac` is 4), "hovering an entity", or "selection could not be cleared" aborts to stage-only with a
toast. All were observed firing correctly during the test, including five consecutive `mode busy` refusals
while the editor was holding.

**Not fixed here, and unrelated:** under deliberately heavy load (several hundred entities pasted in ~2
minutes) the engine raised `"Cannot map buffer with usage BU_STATIC"`, showed a modal and returned to the
map-select menu. That is the engine's documented `level>=6` recoverable path, not corruption, and it was not
reproduced on the fresh-map run. `snapEdit_skipLimits` was **not** set. Cause unattributed; nothing links it
to this code path, and a vanilla session could plausibly reach the same state.

## 2026-07-28 — the staged prefab now survives Play and map changes: it was in the wrong heap

**Supersedes the "Known limitation" of the entry below.** That entry stopped the crash by discarding our
staged prefab on the way into Play. This one removes the need to discard it at all.

**Root cause.** Not structural inequivalence — the object we deserialize is member-for-member identical to
`CreatePrefab`'s. The difference is **which heap its entity-blob array is allocated from.** Read live from
each block's own allocator header:

| block | tag | flags | size | heap |
|---|---|---|---|---|
| engine Ctrl+C clipboard, 1 entity | `0x05` | `0x00` | 432 = `1 × 0x1B0` | **process heap** |
| our staged prefab, 5 entities | `0x05` | `0x04` | 2160 = `5 × 0x1B0` | **map heap** |

Same element type, same tag, different heap. The engine keeps three heaps — global, persist, map — and
`ResetMapHeap` calls `HeapDestroy` on the map heap at map load. So our array's pages were handed back to
the OS while the prefab's `idList` header still pointed at them with `num == capacity == 5`. Measured
directly: after Play the header was **byte-identical** but reading blob byte 0 **faulted**. Unmapped, not
freed — which is the distinction that identified the mechanism, since a freed block's pages stay committed
and read as garbage.

That also explains why the same bug produced two different symptoms. Touch it while still unmapped and you
get an access violation; touch it after unrelated allocations have re-committed that address range and you
get plausible garbage with the heap-owned bit set, which fails the allocator's guard-cookie check and
raises the fatal `Memory corruption before block!`.

**Why the allocation lands there.** Every idlib container asks `Mem_Alloc` for heap id `-1`, meaning
"whatever is on top of `idMemLocal`'s 32-deep heap-scope stack". An allocation's lifetime is therefore
decided by *when* it runs, not by what is allocated — and while the SnapMap editor is up the engine already
keeps the **map heap pushed** (observed scope depth 1). Our deserialize inherited it.

**Fix.** Wrap the stage in the engine's own scope API — `idMemLocal::PushHeap(0)` / `PopHeap()`, reached
through the instance returned by the allocator's magic-static getter. Heap-table slot 0 is `NULL` and
`Mem_Alloc` falls through to `GetProcessHeap()` when the selected handle is null, so pushing `0` puts the
blob array exactly where the engine's own clipboard lives. The engine uses this same `PushHeap(0)`/
`PopHeap()` idiom itself. **No member copying changed.**

`sh_apply_prefab_poll_play()` then stops discarding our prefab — but it asks the block rather than
assuming: `ae_block_survives_map()` validates the allocator's guard cookie and reads the owning heap out of
the block header, returning 0 for anything it cannot verify. So if the push ever stops working, the old
protective re-ctor runs and the user gets the previous behaviour rather than a crash.

**Verified.** Survives a Play round-trip *and* a map change, with Ctrl+V working afterwards — matching the
engine's own clipboard.

**Two things worth carrying forward.**

- *Balance matters.* `PopHeap` **fatals on underflow**, so push/pop are paired with `__try/__finally`.
- *The scope stack is global, not per-thread*, so this briefly changes the ambient heap for other threads.
  That is a **leak** risk, never corruption: every block records its own heap in its header and `Mem_Free`
  reads it back, so a block is always freed into the heap it came from regardless of the scope at free
  time. Also note the whole mechanism is **main-thread-only** — `PushHeap`, `PopHeap` and `Mem_Alloc`'s
  `-1` lookup share a `GetCurrentThreadId()` gate, and off that thread all of it is silently inert.

The three engine functions are resolved by **signature** (`MemLocalGet`, `MemLocalPushHeap`,
`MemLocalPopHeap`), not raw RVA, and each call is range-checked against the DOOM module before being made
— a wrong pointer here would be *called*, not merely read. An earlier revision used a hand-computed RVA
with a dropped digit, which put the object ~256 MB past the module and made the push a silent no-op.

Full engine-side derivation: doom-re `docs/truth/engine/memory-heaps-and-allocator.md`.

## 2026-07-28 — a staged prefab poisoned the paste slot and killed vanilla Ctrl+C / Ctrl+V after Play

**Symptom.** Stage a prefab with Load/Place, press Play, come back to the editor — and the game's own
copy and paste were dead. They stayed dead until you pressed Load/Place again, which appeared to "fix"
them. Vanilla copy/paste with no Load/Place involved was completely unaffected, which is what made this
look like an editor-state problem for a long time.

**What was actually happening.** Ctrl+C was not being refused — it was *running and faulting*. The
shield log showed repeated first-chance access violations at `rip+0x1AB32EE` reading `-1`, classified
`in-editor draw fault -> aborted draw, resumed editor frame`. The engine's copy handler calls
`CreatePrefab` on the shared staging slot at `editor+0x209a8`, and `CreatePrefab`'s first act is to tear
down whatever is already in that slot. Our staged prefab does not survive a Play round-trip, so that
teardown walked pointers the map teardown had already freed. Pressing Load/Place "fixed" it only because
`ae_mkcmd_one` re-ctors the slot before staging, which happens to heal it.

This is **pre-existing** and independent of any Load/Place automation work — it reproduces with plain
stage-only behaviour, and would hit anyone who staged a prefab and then played their map.

**Fix.** `sh_apply_prefab_poll_play()` (polled from the per-tick drain) watches the engine `load_state`
global and re-initialises the staging slot on the way **into** Play, while its memory is still intact.
Re-ctor'ing is the right tool: the prefab ctor rewrites every field unconditionally with no reads and no
frees, so it turns dangling pointers into a clean empty prefab — it leaks the already-dead allocations
rather than double-freeing them.

Two guards matter as much as the fix:

- **Ownership.** We only touch the slot when *we* staged it and the entity count is unchanged. An
  engine-made Ctrl+C clipboard is left strictly alone — it legitimately survives a Play round-trip and
  across maps, and an earlier revision of this fix that cleared the slot unconditionally destroyed the
  user's clipboard and disabled paste. Don't do that.
- **Direction.** Cleaning on the way *back* is too late; the engine can touch the dangling slot first.

**Detecting Play is harder than it looks.** Two obvious signals do **not** work and should not be
retried: the editor session going null (`editor+0x204c8`), and the loaded-map object pointer changing.
Both survive a Play round-trip, so neither ever fired. The engine's `load_state` global
(`base+0x6DDE198`; `3` = RUNNING) does move, and per the fault-shield's own notes an in-editor in-place
load never writes it.

**Known limitation.** ~~Our staged prefab still does not survive a Play~~ — **RESOLVED later the same
day, see the entry above.** The cause was not structural inequivalence at all: the object we build is
member-for-member identical to `CreatePrefab`'s. It was allocated in the wrong **heap**. Note the
hypothesis recorded here — "make `ae_deserialize_to_obj` build an object structurally equivalent to what
`CreatePrefab` produces" — was **wrong**, and chasing it cost several rounds; the members already matched.

## 2026-07-13/14 — SnapStack lives in the backend (`snapstack.c` + `json_patch.c`); the `json_patch` empty-`edit` fix; store slots + management commands

**What & why.** The SnapStack subsystem — the stack-of-stacks + named-group stores and all 20 `sh`
subcommand handlers (`psel`/`popsel`/`phov`/`pr`/`pg`/`pop2g`/`cstk`/`filtinh`/`filtcls`/`bss`/`bsi`/`bsf`/
`bsb`/`bse`/`bsin`/`bscls`/`bsincls`/`accl`/`acctargets`/`mkcmd`) — is hosted **in the backend**, in pure C
(`src/backend/snapstack.c`), backed by a purpose-built dotted-path JSON mutator
(`json_patch.c`) that generalizes `apply_engine.c`'s existing raw-splice technique instead of pulling in a
JSON library (the backend deliberately carries none). Registered from `ui_bridge.c` before the frontend
loads: **one implementation, one store**, and every `kind=0` decl-edit op commits through the one
**synchronous `+0x290` `apply_sync`** path (see the 2026-07-12 writeup below) — no call site can pick the
crashing deferred path. `mkcmd` (`kind=1` prefab paste, a different operation that never crashed) stays on
the deferred staging path.

**A first port (commit `e7ee129`, 2026-07-09) was reset out** — it carried two bug patterns fixed since:
(1) every apply-op scheduled via the
**deferred `+0xd0`** path — the same split-commit-across-threads pattern that double-frees the decl-source
block on map teardown (see the 2026-07-12 deferred-apply writeup below); (2) several persistent `static`
scratch buffers (256 KB × multiple call sites, a 1 MB idstr table) — the same BSS-footprint pattern that
caused the controller-freelook regression on the `+0x298` slot. The shipped port fixes both: synchronous
`kind=0` commits, and all scratch
buffers heap-allocated transiently per call (`malloc`/`free`), never `static`/BSS.

**Store slots (`+0x2A0` `push_to_stack` / `+0x2A8` `clear_stack`).** Two vtable ext slots (ext 7/8) let the
frontend reach the backend-owned stores from the UI: `push_to_stack` wires the Entities-tab "Push to
stack 0" context action to the real stores (dedup on push; multi-select pushes the whole selection), and
`clear_stack` empties stack `index` from "Clear stack 0" instead of needing the DOOM console (`sh cstk 0`)
— it mirrors `h_cstk`'s own logic and returns the pre-clear count so the caller can toast a confirmation.
(An earlier reverted attempt used `+0x290` for push; that offset is now `apply_sync`, so these are fresh
appends — the append-only vtable discipline means no existing slot offset ever moves.)

**The bug that broke every decl-edit driven from the UI — `json_patch` mishandled a non-object `edit`.**
Live-tested `sh bss`/`sh acctargets` both returned `applied 0/1` with no decl change. The engine's own
deserialize `Lexer` rejected the patched text. Root cause (found by dumping the exact bytes fed to the
lexer): an entity you haven't hand-edited serializes its `state.edit` **overrides** block as `null` (or `{}`)
via the `+0xc8` serialize — everything the editor *shows* on it is **inherited** from its decl, not an
explicit override. When `json_patch` had to create a path segment (`targets`, `renderModelInfo.model`, …)
under a non-object `edit`, it spliced a bare `"key":value` **member** where a braced **object value** was
required, producing invalid `"edit":"targets":{…}` — the exact JS-`typeof null` / null-safety class of
gap, one layer
down in C. Fixed in `json_patch.c`: (a) the missing-path branches in `json_walk_set` / `json_walk_upsert_
reflist` now wrap the built chain in braces (`{…}`) so it's a valid object value for both the splice-into-a-
non-object and insert-new-member cases; (b) `json_insert_member`'s empty-object test corrected from
`p == obj_close` (always false — `obj_close` points *past* the `}`) to `*p == '}'`, which had been appending
a trailing comma (`{"k":v,}`) on an empty `{}` edit. Verified offline against a captured real entity plus
null/empty/missing-`edit` variants, then live: `bss`/`acctargets` apply `1/1`.

**New backend-exclusive SnapStack+ commands** (`chkstk` / `chkgrp` / `clrgrp`) + toast polish. Store
inspection/management the OG set lacked: `chkstk [N]` lists a stack (or summarizes all), `chkgrp [name]`
lists a group (or all groups), `clrgrp <name>|*` deletes a group. Also added confirm toasts to previously
silent ops (`cstk`, `phov`, `pop2g`, `pg` receiver/stack naming, `accl`/`acctargets` receiver toast) since a
stack/group op is otherwise invisible without a `chk*`, and a `pg`/`pop2g` single letter-first arg now
implies stack 0 (`sh pg mygroup` == `sh pg 0 mygroup`), consistent with how the operand resolver treats a
letter-first arg as a group everywhere else.

**Verified in-game:** `psel`, `popsel`, `phov`, `cstk`, `pr`, `pg`, `pop2g`, `filtinh`,
`filtcls`, `bss`, `bsi`, `bsf`, `bsb`, `bse`, `bsin`, `bscls`, `bsincls`, `accl`, `acctargets`, plus the new
`chkstk`/`chkgrp`/`clrgrp`/`snapstack_diag`. **`mkcmd` is ported (a faithful C
port of the handler + the byte-exact prefab template) but not live-verified** — flagged as a TODO.

## 2026-07-13 — Palette-Timeline portable-inherit normalize in a backend slot (`+0x298`); the decl-source blob lags the raw inherit by one commit

**What & why.** A Timeline placed from the in-game SnapMap palette is spawned from a repurposed
`snapmaps/editor_only/placeholder_target` entityDef (the only way to make a Timeline selectable in the
palette — the clone can't fabricate one directly), so the fresh entity records *that* as its `inherit`. A
map saved with it only reloads where our override is installed — not portable. The normalize rewrites the
inherit to the portable `snapmaps/unknown`; it lives in the backend vtable slot **`+0x298`
`normalize_timeline_inherit`** (`snapmap_plus_iface.h`/`.c`, `apply_engine.c` `slot_normalize_timeline_inherit`,
exported via `sh_apply_engine_get_slots`), and the frontend calls it from
its Timeline rescan.

**The bug the port exposed — the inherit blob lags one commit.** The normalize kept committing but the
Inherit box (and the *saved map*) stayed on the placeholder. Root cause is an ordering property of the
shared commit body `ae_apply_one`: it runs `DeclSourceRebuild` (which re-emits the decl-source blob at
`defsub+0x38`, baking in the inherit header from the *current* `defsub+0x58`) **before** it assigns the new
inherit to `defsub+0x58`. So after one commit the raw idStr reads `snapmaps/unknown` but the blob still
reads the placeholder — and `get_inherit` (`+0x50`) reads that blob, as does the map serializer. The
normalize's re-fire gate therefore **must read the blob (`defsub+0x38`), not the raw idStr (`defsub+0x58`)**:
gating on the raw field commits exactly once and leaves the blob stuck forever; gating on the blob keeps it
firing across rescans until a later commit's `DeclSourceRebuild` finally bakes `snapmaps/unknown` into the
blob. The
"repeated commits" in the log are load-bearing, not waste. (A raw-field gate was tried first and confirmed
in-game to leave the entity stuck on the placeholder; the DECL_BLOB_OFF gate fixed it.)

**Footprint note.** The slot heap-allocates its two 256 KB scratch buffers per call and frees them;
an earlier version used two persistent 1 MB static/BSS buffers and caused a
**controller-freelook regression** in-game (2 MB of resident BSS) — reverted, and
the reason it's heap-transient now.

> The "freshly-placed Timeline needs a save+reload" belief was never an engine limitation — it was the
> deferral crash plus a frontend JS bug (see below /
> `webview-ui.md`). Both fixed; fresh placement *and* reclass now save immediately.

**Architectural follow-up (since done):** at the time each decl-edit call site independently chose inline
(`+0x290`) vs deferred (`+0xd0`) — a footgun that bit twice this cycle (Save Timeline silently regressed
to the deferred crash path when one line was reverted). The durable fix — SnapStack as shared backend
handlers sending everything through one commit path — landed 2026-07-13/14; see the SnapStack entry above.

## 2026-07-12 — Confirmed: the decl/classname/inherit/displayname Save setters cannot overflow (contributor follow-up)

A contributor asked us to confirm `set_classname` (+0x78), `set_inherit` (+0x80), `set_displayname`
(+0x128), and `rebuild_set_declsource` (+0x40) have headroom for arbitrarily long input — the same
question that, for `PREFAB_TEMP_SIZE`, turned out to be a real stack-buffer overflow (see the
2026-07-06 entry below). Worth checking rather than assuming.

Live-RE against OG SnapHak (Beta 2, `snaphakui.dll`): the **Save to Decl** button's own click handler
(`FUN_1800175a0` → `FUN_180017d00`) reads each Qt field (decl text, inherit, classname, displayname),
computes its exact length, and passes that length explicitly into `FUN_180004a3c` for every one of the
four fields. Decompiling `FUN_180004a3c` shows a standard **growable string assign** (SSO threshold at
15 bytes, ~1.5x geometric growth, allocate-copy-free-old on overflow of current capacity) — the same
shape as `std::string::assign`, not a fixed-size buffer. There is no length cap anywhere in this path;
OG's own Save handler relies entirely on the assign function growing to fit, for all four fields.

This is architecturally unrelated to the `PREFAB_TEMP_SIZE` bug, which was a fixed-size **stack struct**
overflowed by a ctor call sequence — a completely different code shape from a dynamic string assign.
Our clone's `set_classname`/`set_inherit`/`set_displayname`/`rebuild_set_declsource` call the engine's
real assign functions directly (the same functions OG's own Save path calls), so they inherit the same
safety. **No code change needed** — confirmed safe by decompile, not by assumption.

## 2026-07-12 — SnapStack decl-edits double-freed the committed decl-source block; fixed with a synchronous inline apply (`+0x290`)

The long-standing SnapStack crash — run `acctargets` (or `bss`/`bsi`/`bsf`/`bsb`/`bse`), hit **Play**,
then trigger **any** map teardown (reload the same map, **New Map**, or load an unrelated map) and DOOM
dies with `"Memory corruption before block!"` — an access violation at
`DOOM+0x1ab32ee ← 0x19fd162` (`IdStrDtor`) `← 0x17ad00a` (decl-source teardown), `load_state=3`. No save
required; the corruption is in live memory, not the map file. The investigation ruled out the commit
content, the commit function, and a JSON-vs-node-tree architecture gap — each measured live against the
original and found byte-identical; this entry documents the backend mechanism and the fix.

**Root cause — the deferral, not the commit.** Live reverse-engineering of OG SnapHak (Beta 2) proved our
commit body `ae_apply_one` is byte-for-byte OG's `+0xd0` commit (`FUN_180004b80`), and our committed
decl-source blob is byte-identical to OG's. The *only* difference was **when** it runs. OG's SnapStack
handlers commit **inline, synchronously, on the UI/think-loop thread** (the `+0x1a0` work-queue drain).
Our clone ran the command handler on that same UI thread but **deferred** the heavy commit to the DOOM
main thread via `clone_bss_apply` at `ExecuteCommandBuffer` (the old "FIX B", added on the belief that the
structured deserialize AVs off the main thread). That split one atomic operation across two threads and
two frames, and left the freshly-committed decl-source block **double-owned** — so the next map teardown
freed it twice → heap-header corruption → the fault above.

The deferral's premise was stale: our `+0xc8` serialize already runs successfully on the UI thread, and
the reflection context it needs is a process-global singleton (engine `0x17f7030` → vtable `+0x80`),
reachable from any thread — OG proves it by committing on exactly that thread.

**Fix — a synchronous inline apply slot, `+0x290` (`apply_sync`).** Added to the matched-pair vtable ABI
(`snapmap_plus_iface.h`/`.c`, `apply_engine.c` `slot_apply_sync`, exported via `sh_apply_engine_get_slots` and
folded in by `iface_engine.c`). It runs the same per-item batch as the `clone_bss_apply` drain
(kind 0 = decl edit / 1 = mkcmd / 3 = target-write) but **inline on the calling UI thread**, so serialize +
commit are atomic and the committed block has a single clean owner — OG's exact flow. Because the commit
is now inline, callers pass their own item text with no deep-copy/pending store. Each `ae_apply_one` stays
SEH-guarded, so an off-main reflect gap (if it ever occurred) degrades to `applied 0`, never a crash. The
frontend routes all decl-edit ops through it; the deferred `+0xd0`
schedule is kept only as a fallback for an old backend without `+0x290`, and for `mkcmd`/prefab-paste
(`kind=1`), which never crashed and is left deferred. (During the hunt a `C2 SYNC apply: …` backend-log
marker confirmed the inline path ran; removed in the 2026-07-13 diagnostics cleanup.)

This supersedes the earlier "JSON round-trip vs in-memory node-tree edit" theory: our round-trip matched
OG's, so it was never the problem.

**Also migrated to `+0x290` the same day:** **Timeline Save** and the palette-timeline **inherit-normalize**.
The inherit-normalize is a one-shot fired by the UI's rescan
poll; on the deferred path it never persisted reliably, so the poll kept re-firing it (a `tl-inherit-portable`
toast on every logic-entity selection) — synchronous commit makes it a true one-shot. Fixing Timeline Save's
double-free also cleared the downstream timeline symptoms (copy/paste being wiped, needing to "save-backout"
before a timeline save).

> **Convention going forward — commit decl edits SYNCHRONOUSLY.** Any new operation that edits an entity's
> decl (serialize → patch → `ae_apply_one`) MUST commit inline via the `+0x290` `apply_sync` slot,
> NOT the deferred `+0xd0` `clone_bss_apply` schedule. The command
> handlers already run on the UI/think-loop thread where reflect resolves; deferring to the main-thread
> command buffer splits the operation across frames and double-owns the decl-source block. The deferred
> `+0xd0` path is retained only as an old-backend fallback and for prefab/mkcmd staging (`kind=1`), which
> stages into the paste slot rather than rewriting a decl.

**`ae_schedule_target_write` (`kind=3`) migrated to inline too, though it's DORMANT.** It writes
`state.edit.targets` onto the source entity's decl — but it **never fires in normal use**: `sh_target_any`
targets via SnapMap's native input/output-node logic and writes nothing to the decl (only `acctargets` ever
produces a `targets` list). So it was never a live crash risk. It's the natural primitive for a *future*
UI-driven "add target" feature, so it was migrated the same day (`ae_schedule_target_write` now calls
`ae_apply_target_write` inline, SEH-guarded, no deferral) as zero-risk future-proofing — that future feature
is crash-correct by default. Prefab Load/Place + `mkcmd` (`kind=1`) stage into the paste slot (different
mechanism, never crashed, intentionally left on the deferred path). *(Update 2026-07-13: WebView's Save
Timeline was migrated onto `+0x290` too — see the 2026-07-13 entry above; it was on the deferred `+0xd0`
path when this entry was written.)*

**DONE (2026-07-13):** the `AE_APPLY_DIAG` / `AE_DESER_DIAG` flags are now `0`, and the `+0x40 rebuild` and
`C2 SYNC apply` markers plus the `normalize-timeline-inherit … committed` log were removed — the backend no
longer emits the hunt-time diagnostics.

## 2026-07-08 — `apply_engine.c`: `ae_apply_one` could commit an empty class/inherit

Found while root-causing a hard crash-to-desktop / hang on returning to the SnapMap editor after
editing a Timeline, saving, and reloading the map (full investigation, including why the crash
turned out **not** to be caused by this bug, in [`webview-ui.md`](webview-ui.md)).

`ae_apply_one` (the shared `kind=0` commit body behind Save-to-Decl, Save Timeline, and
`wire-target`) deserializes the caller's full patched entity JSON onto a temp def, then copies that
temp def's normalized class/inherit/source back onto the live entity. The class/inherit copy only
null-checked the pointer (`if (clsPtr) ...`), not the string it pointed to — if the engine's own
`StructDeserialize` ever populated the temp def with a **non-null pointer to an empty string** (a
real, observed case, though not the one that turned out to matter for the Timeline crash — see
`webview-ui.md`), the guard let it through and committed `""` onto the live entity's classname or
inherit. A blank class/inherit is never valid; the next full map load fails with the engine's own
`"No class specified"` / `"Couldn't find map entity in entity palette '' inherit = "` and the entity
is unrecoverable.

**Fix:** the guard now also checks the first byte of the string
(`if (clsPtr && *(const char *)clsPtr) ...`, same for inherit) — an empty result is treated the same
as a null one and simply skipped (keep the live value), rather than committed. This degrades a choked
deserialize into "the edit didn't apply, entity intact" instead of "entity destroyed." Universal
across all three `kind=0` callers, not Timeline-specific — a blank class/inherit is never the right
outcome for any of them. Also confirmed to correctly *preserve* a non-standard inherit (e.g.
`snapmaps/editor_only/placeholder_target`) rather than requiring or defaulting to anything, unlike
the original's own Timeline-commit path, which hardcodes `inherit = "snapmaps/unknown"` on every
save (see `fidelity.md`) — the clone's keep-live behavior is strictly safer here.

## 2026-07-06 — `apply_engine.c`: `APPLY_TEXT_CAP` silently truncated large prefabs on Load/Place

Once Load/Place (staging via `kind=1`/mkcmd) was wired up and exercised against real prefab files,
some staged cleanly and some silently failed (backend log: `applied 0/1`, no crash, no fault-shield
entry -- nothing visibly wrong, the prefab just never showed up in the paste slot).

Root cause: `APPLY_TEXT_CAP` (a sanity ceiling on the JSON text carried in a scheduled apply item) was
`256 * 1024` -- sized for small per-entity edits, the only thing this pipeline used to carry. Real
prefab files can run well past that. `slot_schedule_apply`'s batch deep-copy silently truncated
anything over the cap with no error at all, so an oversized prefab got cut off mid-JSON before it ever
reached the deserializer, which then failed to lex the truncated text.

Diagnosed with a step-by-step trace added to `ae_deserialize_to_obj` (gated behind `AE_DESER_DIAG_ON`,
mirroring the existing `AE_SER_DIAG` pattern on the serialize side) -- it pinpointed the exact failing
prefab's text arriving already truncated to one byte under the old cap, confirming the truncation
happened upstream in scheduling, not in the deserialize call itself.

**Fix:** raised `APPLY_TEXT_CAP` to 4 MB, matching the scratch buffer already used elsewhere for
prefab content. Retested against every prefab that had been failing (all `applied 1/1` now) and
live-tested up to 2 MB with no issue -- comfortable headroom over anything hit so far. This cap is also
used as a sanity bound when reading a serialized result back out (`ae_read_idstr`), so raising it
benefits the Create-from-selection direction too, not just Load/Place.

## 2026-07-06 — `apply_engine.c`: prefab create-from-selection crashes

Two independent bugs, both in the `+0xb0` serialize-selection path (`slot_serialize_selection`),
found back to back while root-causing a hard DOOM crash on every "Create from selection" call. This
backend path had never been exercised before the UI's first real call
into it.

### 1. `PREFAB_TEMP_SIZE` undersized — stack buffer overflow

`PREFAB_TEMP_SIZE` (the scratch buffer for the temp `idSnapEntityPrefab`) was `0x220`, sized off the
original's `local_6d8` frame slot. Too small: the ctor at `+0x54d0a0` writes its own fields up to
`~+0x118`, then makes a small forward call into a second, larger ctor that keeps writing fields past
`+0x590` — the real object needs at least `~0x590+` bytes, about 2.6x the old allocation.

Every call overflowed the stack buffer. Because the overwritten bytes land on valid, mapped stack
memory (just not memory meant for this object), it was never a clean access violation, so neither
the fault-shield VEH nor our own SEH guard ever caught it — that's why it crashed DOOM outright
instead of failing gracefully.

**Fix:** bumped `PREFAB_TEMP_SIZE` to `0x2000` for comfortable headroom over the confirmed-required
size.

### 2. `PrefabPopulate` called with 2 args instead of 3 — uninitialized out-param

With the overflow fixed, a second, intermittent crash remained: two distinct locations inside the
engine's `populate()` function (base `+0x54e410`), both a `c0000005 ACCESS_VIOLATION` writing to
near-null address `0x10`, both caught cleanly by the fault-shield (recovers by exiting the editor to
the menu, not crashing DOOM outright): `+0x54e6e7` (`populate()+0x2D7`) and `+0x54f2a1`
(`populate()+0xE91`).

Root cause: `populate()` is actually a **3-argument** function — the 3rd (`R8`, per the Windows x64
calling convention) is an out `int*` status/reason code the engine writes through — but
`apply_engine.c`'s `prefab_populate_fn` typedef and call site only ever supplied 2 args. `R8` held
whatever was left over from the prior call in the sequence (garbage/unmapped, e.g. observed `0x10`),
so the engine's own write through it faulted.

This initially looked hover-state-dependent (an early, wrong theory pinned it on selection
size/complexity instead). That correlation was real but coincidental to the crash: whatever
DOOM's own hover-detection code happens to leave sitting in `R8` beforehand, not anything
`populate()` itself was reading for hover/placement purposes.

**Fix:** added the missing `int *outStatus` parameter to the `prefab_populate_fn` typedef and the
call site, passing a real local variable's address so the write always lands somewhere harmless.

### A confirmed, separate finding: the hover requirement is real

Fixing bug 2 surfaced the engine's *own* validation, previously masked by the crash: status code `2`
means "not hovering entity in selection," and the engine prints that exact message itself
(`"Failed to create prefab: not hovering entity in selection."`) before returning. Create-from-selection
genuinely requires hovering a selected entity in the 3D view — see
[`fidelity.md`](fidelity.md#create-from-selection-requires-hovering-a-selected-entity) — this is not
a clone bug. The webview UI now checks the hovered-id slot (`+0x198`) up front (see
[`webview-ui.md`](webview-ui.md)) instead of relying on the engine's status code, so it can show an
accurate message instead of a generic "nothing selected" one.

## 2026-07-06 — `apply_engine.c`: Load/Place, calling `PasteInstantiate` directly was wrong

First implementation of `kind=2` (Load/Place) staged the prefab (`ae_mkcmd_one`, same as `kind=1`)
then called `PasteInstantiate` (`+0x54f950`) directly, mirroring how `PrefabPopulate`/`PrefabCtor`
are called elsewhere. This looked reasonable but was wrong in a way the crash didn't immediately
reveal: the placed entity came out selected but not draggable, and returning to the editor from Play
afterward crashed DOOM hard (`ACCESS_VIOLATION` reading `0x0` at `+0x5a516b`, a large call stack
through several engine frames).

Root cause: the engine's real native Ctrl+V handler (`+0xce1810` in `DOOMx64vk.exe`) calls
`PasteInstantiate`, then calls a *second* function (`+0xcf35e0`) that sets grab-tool state (bit
manipulation at `obj+0x2d0`/`+0x2d1`, `obj+0x1ac = 4`, `obj+0xbb8 = 1`) on some object passed into the
handler as its 2nd argument. Calling `PasteInstantiate` alone skips that second call entirely, leaving
the engine in whatever inconsistent state that omission causes — undraggable placement now, a crash
on world transition later.

Identifying *which* object needs those writes turned out to be a dead end via live debugging:
interactive breakpoint-based verification (attach, set a breakpoint, `debugger_continue`, wait for a
user action to hit it) is **confirmed broken** in this environment — it reliably freezes DOOM's input
even though the debugger reports the process as running, regardless of what triggers the breakpoint
(tried 4 times, different breakpoints, different triggers). A static hypothesis (the object is the
same selection object behind `hovered_id`) didn't hold up against a read-only memory check either —
the candidate field region held array-like data, not simple flags.

**Fix — sidestep the problem entirely:** confirmed against the *original* `snaphakui.dll` +
`XINPUT1_3.dll` (static Ghidra decompilation, no live debugging needed) that neither ever calls
`PasteInstantiate` directly either. A prefab double-click in the original UI
(`snaphakui.dll!FUN_180017538` → `FUN_180013878`) only reads the file and stores its text; the actual
"deserialize into `editor+0x209a8`" step is a separate function (`XINPUT1_3.dll!FUN_180006bf0` →
`FUN_1800094f0`) — the same operation as our own `ae_mkcmd_one`. Placement always happens through a
real, native Ctrl+V; nothing in either original DLL calls `PasteInstantiate` at all. So `kind=2` no
longer calls it either — it just stages (`ae_mkcmd_one`, identical to `kind=1`), and something else
finds the game's own top-level window (`EnumWindows`, same-process, skipping our own
`"SnapHakStudioWebView"` companion window), brings it to the foreground, and synthesizes a real Ctrl+V
via `SendInput`, letting the engine's own already-correct native handler do the actual instantiate +
grab-tool setup, exactly as it would for a real user paste — no need to ever identify the mystery
object at all.

**First attempt at the synthetic-input step was on the wrong thread.** Put the `SetForegroundWindow` +
`SendInput` call directly inside `ae_mkcmd_and_place`, which runs on DOOM's own main thread (inside the
engine's `clone_bss_apply` command-buffer drain). Staging succeeded (confirmed via the toast, "applied
1/1") but no paste ever happened, even though the user could then paste manually with Ctrl+V — proving
the staged data was valid the whole time. `SetForegroundWindow` requests a focus change but doesn't
apply it synchronously; the window that needs it processes the change on its own message pump. Calling
it from DOOM's main thread and then immediately (same thread, no yield) calling `SendInput` fires
before that thread ever gets to pump the message that would complete the focus switch — so the
keystroke landed nowhere. **Fix attempt 2:** moved the whole window-find + focus + `SendInput`
sequence out of `apply_engine.c` entirely and into the webview UI's own think-loop
(`poc_synthesize_native_paste` in `snapmap_plus_ui_webview.cpp`), which has its own message pump and isn't
blocking DOOM's simulation. Also deliberately delayed ~6 loop iterations (~200ms) after a successful
schedule, plus a 50ms sleep between the focus request and the keystroke, so the engine's command-buffer
drain has actually staged the prefab, and the focus change has actually landed, before Ctrl+V fires.

**That fix moved the keystroke correctly but surfaced a third side effect.** Staging still succeeded,
and this time the focus switch itself visibly worked -- but forcing `SetForegroundWindow` on DOOM's
window made the game pop its own ESC/pause menu, almost certainly a "regained focus after being
alt-tabbed away" safety behavior (our webview panel is a separate top-level window, so from the game's
perspective, forcing focus back to it looks exactly like the player alt-tabbing back in). The
synthesized Ctrl+V then likely lands on that menu instead of the 3D view, so nothing pastes -- but
again, the staged data is untouched and a manual Ctrl+V still works once the menu is dismissed.

**Decision: stop automating this step.** Three attempts, three different side effects (a hard crash, a
silent no-op, an unwanted pause menu), and each only surfaced by actually testing in DOOM. Given how
consistently fragile this specific corner of the engine has been, `kind=2` was removed entirely --
Load/Place is back to plain `kind=1` (identical to the `sh mkcmd` staging path): stage the prefab, then tell the
user to press Ctrl+V themselves. This matches the *original* SnapHak's own actual workflow exactly (see
the double-click investigation above) and needs zero window-focus tricks, zero risk of the side effects
above, and zero remaining need to ever identify the mystery grab-tool object.
