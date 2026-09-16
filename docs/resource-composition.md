# Resource composition architecture

This is the target architecture. The current compiler merges all contributions
together, including new nested objects and compatible collection ordering, but
still selects collection identities from a small rule table. That table is not
complete coverage of the game's collections. The shared composer now accepts
the dependency walker's native schema and handles sparse fixed arrays with
extent validation. Runtime entityDef and ordinary reflected declaration roots
use that schema. The registered graph reader has an ordered composition adapter,
and MD6 definitions compose their verified named-record envelope. Other custom
root grammars and general index-reference relocation remain open.

Entity source-class resolution now follows a caller-selected parent source view
and verifies the native class ancestry. Missing parents, unknown classes,
incompatible explicit classes and cycles refuse without creating native defaults.
Runtime uses this resolver for each input and the result; it does not expand
inherited property values. Resource-family emission places entity headers
in the native parser's order, including headers introduced by a contribution.

Custom readers receive the same source ownership views: verified original,
the outer package owning one contribution, or all contributors to the result.
Nested components remain part of their outer package. These reads return raw,
integrity-checked inputs; they do not select a peer, load native resources, or
certify a composed dependent resource. A reader projecting a dependent schema
must validate that schema in each view and retain its semantic dependencies.
Entity parent metadata now uses this shared service. Reads use the compiler's
canonical path index, so resolving one dependency does not scan every source.

Overlapping native materials now use source-bound renderparm schemas and an
ordered write composer. Independent full-vector components combine; expressions,
masked writes and opaque values retain their native text. Final schema and
prior-write bindings are checked before accepting a merge. Equal schema copies
coalesce; incompatible schema definitions refuse. A new schema may come from a
peer only when neither the original nor the owning package supplies it.

Native reference constraints join authored collection ordering before the
compiler breaks ties between independent additions. A material expression that
reads a parameter's default stays before a new peer write to that parameter.
Repeated-write prefixes stay intact. Opposing constraints and cycles refuse;
authors do not supply ordering settings.

Candidate source inspection retains material parameter and table sources, and
collects typed image/program references for catalog resolution. Parameter defaults
are inspected too, including image/program defaults and named buffer struct
definitions. Null defaults, buffer sizes and image-store formats do not invent
file dependencies. Material String values, sampler paths, custom buffer values
inside material blocks and late consumers still need additional adapters.
Single/equal custom-resource passthrough remains supported; these checks do not
establish complete native activation or dependency coverage for every material.

Resident reconstruction completes virtual-texture material binding inside the
verified process-heap scope before consumer updates. A permanent material must
not keep binding arrays allocated from a heap that the engine destroys on map
exit. This lifetime requirement applies to reconstructed stock materials as well
as package additions.

## Author inputs

Installed game archives are read-only inputs. "Replacement" denotes the runtime
resource served for an engine identity, never a write into `snapgameresources` or
another installed archive. Whole binary resources use complete override payloads;
their original bytes and the author's source files remain unchanged.

Authors supply `package.json` and `assets/` using engine paths. Requirements,
strings and other package policy remain in the descriptor. Resource schemas,
entry identities, collection modes, priorities, hashes and file mappings are
compiler concerns and do not become package settings. Delivery preserves each
authored package, including shared resources.

## One resource model

Installed game originals, product defaults, package contributions and generated
map resources need explicit identities and provenance in one resource model.
The dependency walker and typed composer use the same field/reader schema. File-open
hooks should serve a validated result rather than independently choosing among
conflicting producers. Generated navigation and private Grid Room resources are
producers too; registering their identities does not mean appending their binary
bytes or embedding the product itself in every map.

Grid Room transforms now read their stock inputs from the published compilation,
then product defaults and installed resources. Navigation's base reader consumes
that Grid output where applicable and otherwise the same effective source view.
These reads stay below navigation generation to avoid recursion. Claimed package
read failures do not select a stock fallback. Grid output records its source
paths even when requested by a preview outside a native resource parse.
The final package-publication boundary holds the navigation consumer lock before
swapping the compiled provider. It retires navigation previews and traversal rows
before releasing readers; worker results from an older revision cannot republish
them. Compilation and cache preparation happen before this short boundary. A
failed activation releases the boundary and retains the previous provider and
derived caches. Lock order matches input reads: navigation before compilation,
with traversal invalidation after the compilation lock is released.

Navigation preview tasks retain their exact base-resource path and whether a
usable traversal table supplied that revision. Those dependencies publish with
the cached bytes, including when reuse skips all input reads. Resource graphs
store each product stage's input contribution separately from native parse edges
and other producers. Replacing a stage drops its obsolete dependencies; source
reload clears stages until republished. This also preserves Grid input edges
when navigation and its base are requested under the same file name.
Private Grid Room outputs now enter the compiler as producers. Their originals
are generated using installed inputs; their effective contributions are generated
using the prospective compilation. Dependencies compile before their consumers,
including resources whose paths sort later. A cycle or unreadable input refuses
the prospective result. This build never reads the live modded provider.

Private module, module-info, geometry, probe and AAS aliases share one compiler
identity. Package files retain their authored paths and bytes. Identical copies
coalesce only in the runtime result; disjoint supported declaration edits compose
against the generated original. Different opaque output/input edits report both
the direct contributor and contributing input packages. A static default and a
dynamic producer cannot independently claim the same identity.

Compiled generated outputs retain their input paths and package ownership. The
file hook serves those outputs before considering on-demand generation. Removing
a direct output override recompiles that identity with the remaining inputs;
removing every contributing package restores product output without package
ownership. Readers holding older byte snapshots retain them across publication.
Grid module text remains opaque unless a verified declaration adapter identifies
its grammar; this work does not infer merge semantics from a `.decl` suffix.

Complete navigation producer staging and native resident geometry/image
retirement still require integration. These
compiler/provider tests do not establish renderer acceptance of hot replacement.

Built-in declarations enter the same compiler pass as intact package inputs,
including identities with no package override. The runtime no longer compiles
packages first and then recomposes individual built-ins. Built-in declarations
participate in original-relative composition. A product
declaration absent from the game supplies its own original. Built-ins contribute
no package ownership. Their editor functionality must not turn vanilla Unknown,
Timeline, Lift or blocking-volume maps into package-dependent maps.

## Collection semantics

The typed composer resolves the baseline and each contribution under its own
state type for the `edit` block. It merges scalar declaration headers first,
then resolves the result type before composing state. Its caller supplies the
original, per-package and effective parent source views; reading whichever
parent happens to be loaded would lose the distinction between these inputs.
This per-input path is covered by offline tests and used by runtime entityDef
composition once native reader registration has completed.

Non-entity declarations using `idDeclTypeInfo` now use their native declaration
class for the `edit` block. This includes editor, AI-component, sound, effect
and other reflected families. The two executable inventories contain 335 such
families; their common parser and post-parse wrappers were checked separately
in each image. Compiled inheritance precedes state, as the native parser
requires. This establishes root field types, not semantic identities for every
collection or complete knowledge of every custom field reader.
Of those roots, 324 use ordinary reflected objects and 11 register custom
readers. The latter remain atomic until their own composition contract is known.

The shared graph reader now has a separate ordered syntax and topology view.
It retains repeated subgraphs, nodes, links and opaque object fields, validates
the native envelope order, and resolves endpoints by the engine's exact first
node-name match across subgraphs. Link groups must already resolve while their
subgraph is read; final endpoints may refer forward. Layer names resolve to the
native 32-bit masks. A repeated node or transition name is not a unique merge
identity, even when both link endpoints also match.

The graph dependency adapter binds that reader through native ancestry and
reader registration, validates each object's class, and walks each occurrence's
reflected state. It preserves known references when other readers are unsupported
and reports those gaps. These APIs support graph composition but do not establish complete
gameplay dependency closure. The generic assignment parser still rejects repeated
fields. Only the graph envelope uses the ordered parser, and repeated ordinary
assignments cannot be reported as complete dependency coverage.

The graph composer lowers the verified envelope into a private typed view and
uses the same original-relative, all-contributor merge as ordinary declarations.
Root object state bypasses only the graph's own custom reader, matching native
dispatch. Subgraph, node and link objects use their registered polymorphic class
readers. Layers and named configurations retain compatible ordering; repeated
records retain occurrence slots within their own collection. If duplicate
multiplicity changes, the common slots must remain unchanged; an ambiguous
simultaneous insertion/removal and edit refuses composition. Identical repeated
records inside one contribution remain repeated. Copied contributions do not
multiply them again.

Links carry private references to their source node occurrences through the
merge. The graph's first-name lookup table is composed too, including graphs
without links. After native serialization, the graph is parsed again and every
endpoint and name lookup must resolve to the intended occurrence. A new duplicate
name cannot silently redirect another contribution's reference. Internal identities disappear from
the served text; authored package bytes remain intact. Ordinary nested fields
still follow their own schema: this envelope adapter does not invent semantics
for unknown custom readers, inherited state or late consumers of strings.

MD6 definitions use a separate envelope adapter. Joint groups match by native
category and name, animation event blocks by normalized animation path, and
animation aliases by name. Compatible record additions, removal and ordering
compose against the original, including built-in contributions. Repeated events
inside one animation block remain repeated; their complete block is currently
atomic. Group payloads and other custom sections retain their native
syntax rather than being interpreted as assignment dictionaries. Independent
changes to different records combine; divergent edits within the same opaque
payload explicitly conflict.

Inside an animation alias, animation references and flag blocks compose in
authored order. Existing animation slots retain their original positions, so
incompatible edits of one slot conflict. Newly appended slots use the native
interned spelling plus their occurrence, preserving repeated variants. Equal
appended occurrences from multiple packages are applied once. Interior edits
are compared at their native positions; no inferred relocation is performed.
Flag tokens retain their spelling and multiplicity;
the native reader recognizes `forceLoad` and ignores other flag tokens. Changing
flags and adding animation variants can therefore compose within the same alias.
Removing a flag block conflicts with another contribution changing that block.
These are collection additions/removals, not arithmetic on animation counts.

MD6 initialization composes ordered setters for inheritance, mesh, offset and
bounds calculation. Independent setter edits combine; repeated writes retain
their order. Inheritance also writes the model, offset and bounds flag, so the
compiler checks the final writer for each affected value and diagnoses an edit
that another contribution would silently overwrite. Individual offset vectors
remain atomic. The author files retain their original bytes.

MD6 emission follows native section order, including optional base user channel
and rig sections. Group copies must resolve an earlier local group in each input
and in the emitted result. Duplicate group/alias/event-block identities refuse
composition instead of relying on native shadowing or error behavior. A change
to the selected model or parent cannot silently receive another contribution's
records authored against a different binding. This is direct binding validation,
not inherited-state expansion or validation of skeleton joints, animation event
arguments and nested custom data. Unsupported obsolete syntax and escaped record
identities remain explicit gaps. A heap-based delimiter stack handles deep opaque
payloads without adding an author size/depth setting.

Materials and other custom root grammars still need their own composers.
One complete replacement or exact duplicates retain their original bytes.
Overlapping changes to an unsupported family fail explicitly. Alternate native
source dialects, including legacy particle syntax, also remain intact when no
composition is needed. The typed path cannot reinterpret them as reflected
assignment syntax.

The package compiler supplies these views from the complete source inventory,
including parents that sort after the child. A contribution sees the original
and its outer package's components; the result sees all packages and built-ins.
Parent headers are composed without interpreting parent state. Unknown or
conflicting parent classes fail the prospective compilation. The runtime
supplies the native schema after checking the game's completed reader setup.

Initial compilation runs on the main thread immediately before the engine's
whole-registry promotion. Native reader registration precedes this boundary in
both supported executables. The provider and publication hook are armed first;
compilation, policy refresh and declaration publication then complete before
the native promotion. Runtime rearm uses the same compiler at its settled
browser boundary. Failed compilation leaves the prior snapshot intact.
Local and built-in strings are installed earlier because built-in declarations
can request them before this compilation boundary. Package strings join only
from a published policy; a failed refresh still uses the previous policy.
Runtime rearm updates existing dictionary rows instead of inserting duplicate
keys, including when a package replaces a vanilla label. It retains the
original native record for restoration. Once a complete string pass succeeds,
absent package-only keys leave the dictionary and displaced vanilla records
return. A remaining user or baked definition keeps the key active. Retirement
validates ownership before compacting the list, preserving unrelated entries
and native ordering. String pointers belong to the engine pool and are never
freed by row removal. Provider recovery uses this same pass to undo introduced
keys and restore previous values; the native lookup index is rebuilt each pass
and follows row indices across list growth rather than retaining array pointers.

Before publication, `sh_package_compilation_changes` compares the previous and
prospective effective providers. The result owns a sorted list of added,
replaced and removed paths. Package identities, ownership, duplicate counts and
cache filenames do not produce false changes. Memory-backed resources compare
all bytes; opaque snapshots compare their captured SHA256 and 64-bit lengths.
Mixed representations use a verified bounded stream. This comparison describes
captured content, not current source availability or native readiness.

Activation callbacks receive that list outside the provider lock. If activation
fails, the previous provider is restored and the recovery callback receives the
opposite direction without allocating or rereading source files. Empty changes
still run activation because policy and native readiness are separate concerns.
The declaration pass currently retains its full refresh behavior.

Native replacement also needs the consumers of each changed provider file.
`sh_resource_graph_affected` snapshots that reverse closure across source,
entity-state and product-stage edges. The result owns its normalized identities
after the graph lock is released; it includes intermediate generated files and
typed resources, coalesces duplicate inputs, and terminates on cycles. It is a
set for an activation pass, not a dependency reload order. Missing input records
or incomplete/active graph nodes return incomplete coverage with the known set
retained. A complete result covers the recorded graph only; native registry
coverage must be checked separately before claiming every resident consumer.

This query prepares consumer selection; it does not yet implement native asset
retirement. The verified native MD6/image reload callbacks use source metadata
and can invoke source-to-cooked-file generation. Static model reload explicitly
skips names containing `.bmodel`. Those callbacks cannot serve as a blanket
package installation step. Cooked resource loading, renderer synchronization,
consumer reconstruction and per-resource completion remain distinct activation
requirements. Package authors do not configure these engine operations.

Compatible reflected objects retain independent field edits when their class
changes. Each input and the final result validate fixed-array bounds separately.
Equal scalar text under different native reader types is a replacement, not an
unchanged value. A previous custom-reader value cannot supply defaults for a new
object type, and incompatible edits report their contributing source and field.
Fixed arrays preserve their
native sparse indices without inserting `num`, filling missing slots or
renumbering entries. Known dynamic collections remain atomic unless a verified
identity contract is supplied, even when their text omits `num`. Custom readers
and unresolved compound fields do not silently become ordinary object merges.
The metadata adapter may be opened without a dependency observer for composition;
attempting dependency emission without one explicitly aborts.
Native string and atomic-string readers are recognized by their registered
reader identities. Interning text alone produces no typed resource edge. Later
consumers that interpret that text as an asset name still require their own
adapters; successful typed inspection does not prove late gameplay closure.

Native `idTypeInfoObjectPtr` values now share a class-aware reader between
composition and dependency inspection. The adapter requires matching registered
readers across template instantiations, reflected element metadata and verified
class ancestry. Both scalar class names and `className` blocks are supported;
the optional `object` body uses the selected class's own field and reader types.
Independent edits within that body compose, including compatible class changes.
Conflicting class selections, incompatible field readers and deletion versus
modification report the affected field. Nested custom readers remain atomic.
Empty and `NULL` forms depend on native destination state and remain opaque;
they cannot silently become empty objects. This adapter handles declaration
syntax; JSON polymorphic state still reports an unsupported-reader gap.

Every encountered collection needs a resolved contract. Native reflection can
establish element types, container readers, fixed extents and serialized count
and item names. It does not establish all semantic identities or index uses.
Verified contracts should attach to reusable element/container types or native
consumers, with field-specific semantics only where the same type has different
uses. A field name such as `name`, or uniqueness in sample files, is evidence to
investigate, not sufficient proof of identity.

| Collection contract | Composition |
| --- | --- |
| Fixed positions | Preserve extent and position; merge independent edits within each position. |
| Entries with stable identities | Match identities; retain additions; recursively merge each entry; distinguish removal from unchanged data. |
| Ordered sequence | Preserve each contribution's ordering constraints and original anchors; reject incompatible ordering. |
| Index-linked graph | Merge stable node identities, allocate final indices, then rewrite every verified index reference and validate the graph. |
| Unknown/custom reader | Report the unresolved type and field; do not invent identity, reorder entries or silently select one contributor. |

Equal edits coalesce. Different fields of the same entry merge. Contradictory
scalar edits conflict; values such as health or animation timing are not summed.
Deletion versus modification is a conflict. Identical opaque resources share
runtime storage; divergent binary replacements require a format-specific
composer or an explicit incompatibility report.

## Compilation and activation

1. Resolve native resource identities and verified baselines.
2. Parse supported resources into typed data and compute each contribution's
   additions, edits and removals relative to its baseline.
3. Compose using the shared schema, retaining contributor provenance down to
   fields and collection entries.
4. Resolve ordering and reference relocation, then rebuild counts and indices.
5. Validate types, references, resource dependencies and package policy before
   emitting the game-facing payload.
6. Publish the prospective provider for activation while retaining the previous
   compilation, catalog, immutable resource files and selected map policy. Map
   admission remains closed until the consumer pass succeeds. A failed compile
   keeps the previous result; interrupted installation discards the pending install.

Both boot publication and browser refresh now use the runtime activation
transaction. A failed declaration/policy pass or native exception restores the
previous provider before running consumer recovery against those retained bytes.
Recovery does not rescan authored files. Both swaps invalidate derived source
caches, and native calls run outside the provider lock and cache boundary. Map
selection cannot change during the transaction. Even successful recovery leaves
the attempted install unready; a later successful refresh admits it. Recovery
failure is reported separately from the original activation error.

Prepared package sound shaders select required audio banks by their native event
IDs. The activation catalog covers the bank root and the single language
directory the engine mounts, and takes each bank from its effective installed
original, so a mounted package takes precedence over a loose file and a second
locale never contributes events the engine would not present together. Bank file
names are accepted in both native spellings: the folded name hash and a decimal
identity. Cooked bank records are read without loading embedded media.

Wanting a bank also wants the banks its play actions name, which those records
state outright. Compiled package banks replace installed ones by whole-file
identity: an already-loaded bank with different or unknown bytes is released and
reloaded, a displaced bank the game owned is restored even after a later load
fails, and an unchanged bank is never cycled. Every load precedes every release,
so a dependency is resident before the bank that held the previous one goes away
and a transaction interrupts no voice it does not have to.

A changed media payload reaches the engine through the file provider. One that a
loaded bank carries in its own data is named in the activation log instead of
being half-applied, because the bank supplies those bytes itself. Activation also
logs unresolved structure references, packaged bank identities with no native
name, and how many changed media each case covered. Provider recovery restores
the prior selection and releases only loads owned by the failed activation. No
audio-system restart or author dependency list is required.

References a bank cannot satisfy itself are split by native action kind. A play
action's target and an event's own actions are hard, so anything missing from
those refuses the pass with the bank name and the object identity. Every other
action kind is a control action whose target may legally be absent: stopping or
muting an object that no loaded bank defines is a no-op, and loading unrelated
banks to satisfy one would embed content the map never plays. A play target
another bank owns is attributed to that bank, not counted missing.

A changed media payload that a loaded bank carries in its own data -- resident,
or the prefetched head of a streamed source -- cannot be replaced by overriding
the loose file alone, so activation refuses with the media path, its identity and
the carrying bank. Re-cooking a bank to embed new media is the game's audio build
step, not this compiler's.

Bank replacement only happens at a package-activation boundary, which is either a
map transition or the settled browser with an inactive editor and no loading. No
gameplay world is playing voices there, so no voice is interrupted by a
replacement in the first place.

Remaining audio limits: a bank identity present only inside a mounted package has
no name the loader can open, and is counted rather than activated; differing
packed candidates still refuse as an ambiguity instead of modelling mount order;
and external-source requests stay native.

The audio file adapter now resolves native numeric and named bank/media opens
through the active immutable package provider, before the native packed or loose
file fallback. Paths retain the engine's bank/media prefix and language rules
under `assets/sound/soundbanks/pc/`. Named external-source absolute requests and
write requests retain native behavior. A claimed but unreadable package file
fails instead of falling through to a different local copy.

On success the adapter gives native code a duplicate OS handle to the verified
snapshot. Native offset reads and close operations own that handle; an existing
stream retains its original bytes across provider changes. The two open slots
are installed with atomic pointer exchanges after checking their independently
resolved methods and constructor table on both executable images. Unit coverage
includes failure paths, language selection, two concurrent snapshots, and a
sparse file larger than 4 GiB. This is file-routing coverage; the activation and
recovery described above is what replaces a loaded bank. An open-file adapter
cannot certify a native voice or stream transition on its own.

This supplies provider rollback around the declaration, policy and installed
audio-bank passes.
It does not yet undo every native side effect: newly registered declaration
identities, resident opaque resources and renderer-owned consumers still need
their lifecycle handling. Dictionary key retirement and vanilla-record
restoration now participate in consumer recovery. The filesystem installation
batch remains provisional through consumer activation and commits afterward.
Cancellation or activation failure rolls the provisional batch back as a whole.
Complete package activation and delivery acceptance remain release requirements.

Coverage is a release criterion: known syntax is not the same as known merge
semantics. Tests must exercise real resource families, additions at colliding
serialized indices, nested lists, reordering, graph references, removal, built-in
coexistence, both renderers and map delivery. Unsupported cases must be visible
in the coverage inventory rather than described as general append support.
