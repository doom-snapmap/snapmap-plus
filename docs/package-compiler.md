# Package compiler

Implementation and live acceptance are in progress. This document describes the
author format and the compiler boundary; it does not claim completed runtime
installation or renderer validation.

A package is one folder with `package.json`. Its `assets` directory mirrors the
engine paths reported by an extractor. Authors do not write file lists, hashes,
installation receipts, resource manifests, path mappings, or version fields.

Installed game archives, including `snapgameresources`, remain untouched. A
resource replacement is an override served at the same engine path: source files
stay inside packages, and compiled bytes stay in the separate cache. Whole binary
resources are selected as complete payloads rather than appended together. This
does not overwrite or repack the original game files. A map's resource overrides
last only for that map's active resource view.

```text
overrides/
  cyberdemon/
    package.json
    assets/
      generated/
        decls/
          entitydef/ai/...
          material/...
          md6def/...
          renderprog/...
          sound/...
        basemodel/...
        skeleton/...
        image/...
        spirv/...
        renderprogs/...
      cooked/
        model/...
        anim/...
      md6/...
  boss-demons/
    package.json
    assets/...
```

Start with one descriptor and one `assets` directory per package, even when it
contains several demons. Copy each extracted file beneath `assets` using its
engine path. The compiler discovers the files; adding another resource does
not require editing a file list in `package.json`.

| Location | Contents |
| --- | --- |
| `package.json` | Package identity and description, requirements, localized strings, and supported HUD rules. |
| `assets/` | Declarations, models, textures, shaders, animations, sounds, and other engine files at their original paths. |
| Other authored files | Optional documentation and supporting files, preserved with the package. |
| Saved map | Complete compressed copies of the packages selected from gameplay asset use. |
| Application data | Compiled output, content checks, and installation bookkeeping, maintained automatically outside authored packages. |

These paths illustrate several resource families. Keep the extractor's actual
path, including its extension and shader permutation directories. A `.bmodel`
under `cooked/model`, a `.bmd6model` under `generated/basemodel`, and an MD6
declaration under `generated/decls/md6def` have distinct roles. Moving them into
a common `models` folder changes the path requested by the engine.

## Map content and the local library

Saved-map admission uses compiled resource availability. The compiler checks what
the game and installed providers can supply, including equivalent resources in
a differently named package. Package IDs organize authored content and delivery;
they do not establish that all required resources are available.

The compiler now provides a separate map overlay: map-authored resources take
precedence at their engine paths, while other local resources remain available.
This applies to declaration text and opaque resource bytes alike. Map policy
takes precedence over matching local policy values. Each map's peer packages
still compose normally and report contradictory edits within that map.
Product defaults without map contributions do not hide local editor edits.
The overlay retains every authored source member and independent ownership;
creating or freeing it does not write the installed package trees.

The runtime retains the local library separately from its effective map view.
Switching maps uses that captured library unless a committed installation has
changed its sources. A commit marks the library for recompilation from its data
root before the map overlay is next composed or retired. A failed rescan keeps
the current provider and retires nothing, so newly installed resources cannot
disappear when the temporary map view ends.
Refreshing local packages while a map is active updates the library while
retaining the map's resource precedence. Provider publication and native callback
failure restore the previous library, map view and selected policy together.
Leaving the map retires its selected policy and restores the local provider;
already-open opaque streams retain their bytes. Saved-editor activation and
browser retirement have passed live OpenGL checks; other entry points and
already-loaded opaque resource consumers still need acceptance.

Saved-map loading now prepares a retained compilation at the native
`EditorLoadMap` entry, while the editor is inactive and the browser is settled.
Preparation publishes nothing and does not change runtime readiness. Once
admitted, the same candidate is activated and the parser receives the exact JSON
read during preflight. Returning to the browser restores the local provider
before releasing private map sources. Failed activation restores the prior
provider; a failed cleanup retains ownership for retry. In-place editor reloads
and transitions from a running world still require their own teardown boundary.
Saved-map preflight checks the full compiled payload against local availability.
When every supplied path is present, differing package names or bytes do not
request installation. The candidate still requires successful native activation
before parsing. Missing-payload consent offers the intact delivery owners that
supply missing resources. It retains the exact map JSON, private source trees,
compiled candidate and saved-map identity while the native prompt is pending.
Acceptance publishes the complete install group, activates the candidate, commits
installation and continues that original load. Decline or a superseding request
cancels the pending load. Preparation/activation failure cancels the whole new
disk group. No forced reload or game restart is requested. This saved-map route
has unit coverage and live OpenGL acceptance for decline, installation from an
empty folder, automatic continuation and subsequent loading without another
prompt. Installed outer packages matched every embedded file and directory.
Other load entry points and complete current-build Vulkan acceptance remain open.

The retained-request core also accepts an owned native launch context. It copies
the checked JSON and rechecks that the same request remains valid through consent
and lobby waiting. Preparation preserves the current provider. Abandoning
the selection cancels the whole pending installation and releases its owner.
Published offline Play, lobby creation, direct launch and joining-client cache
readiness now enter this shared request flow. They deep-copy native metadata and
retain the original cached JSON instead of keeping temporary download objects.
The source and metadata adapter has live OpenGL inspection coverage; complete
published launch and multiplayer acceptance on the new backend remain open.

Published caches can be read through the native profile filesystem before any
deserialization. The source adapter retains independent bytes and restores native
buffer and allocator ownership on success and failure. Its full-byte OpenGL check
uses an existing downloaded map. A separate thread-local inspection scope permits
metadata decoding without installing packages, selecting map policy, consuming a
rawmap arm or changing the active map's save/render state. Delivery variables are
stripped from these temporary objects; actual launches retain the original source.
The native published reader and asynchronous completion are bracketed by this
scope. Their hooks and all four launch gates install as one group; partial
publication stays in pass-through mode. Inspections cannot activate a package.
Retained launches wait for any enclosing inspection scope to unwind, including
when native download callbacks pump menu maintenance. Nested native launch calls
reuse preparation only for the same retained map identifiers and menu owner;
an unrelated selection must pass its own preflight.
Pending launch validity includes the native selected identifiers, menu owner and,
for joining clients, the host's current map selection. Superseding a request
cancels its whole pending installation and clears its native consent prompt.

Published session setup constructs and serializes an empty native map, then copies
only the authored four lobby slots into it. The native decoder requires every
default field, so a partial map document is insufficient. This temporary snapshot
contains no authored entities, delivery data or inherited resource state. It is
never saved or used to build gameplay. After the requested native unload/purge,
the map provider activates and the retained full source is freshly decoded into
the selected cache, before native SnapMap conversion. Local parent declarations
therefore cannot bake their values into that final snapshot during session setup.
The deferred installation commits only after native map finalization succeeds.
Failure cancels the entire pending install and follows native map cancellation.
Unrelated resource refreshes wait while this request owns the installation.
Native session decoding and early/late cancellation have live OpenGL checks through
temporary adapters; full published/network acceptance remains open.

The initial published read uses this same resource-free snapshot when the source
contains a package carrier. Admission is limited to the first native parse from
the verified reader call site, on the engine thread. Nested callbacks and later
full-map reads cannot reuse it. Carrier-free vanilla maps retain their normal
decode. This lets metadata and lobby setup reach the launch gate even when a
parent declaration has not been installed. The edit/clone callback retains the
source identity rather than saving that preliminary snapshot. Both executable
call sites pass image checks; live OpenGL native decoding covers a deliberately
missing parent and unchanged retained source bytes. Full UI clone/edit and
published download-to-consent acceptance on the current build remain open.

Published admission uses current selection and menu ownership; pure preparation
does not require an inactive editor. The subsequent native handoff requires the
exact retained level request, main thread and completed resource purge. Saved
editor admission still requires its settled browser boundary. A queued launch
retains its provider until real departure from the browser, and a waiting lobby
keeps its selection without an arbitrary timeout. Browser return retires that
selection. Full active-world and in-place editor acceptance remains open; menu
state tests alone do not establish coverage of those engine paths.

The resource availability service now checks a resolved set of canonical engine
paths against an independently indexed installed source inventory and verified
SnapMap/product originals. A post-install inventory publishes and rolls back with
map activation. Storing overlapping variants does not compile all their values
together or change the retained local resource provider.
Package names and byte differences do not affect availability. Campaign-only
archive presence is not sufficient for SnapMap. Missing files select their whole
map delivery owners, including duplicate suppliers; a missing file with no
supplied source is an error. Product-generated outputs expand to their recorded
input paths. Repeated requests are deduplicated before probing files.

Full payload coverage is a sufficient no-install check, not a claim that every
supplied file is used. It includes all authored resources and generated inputs,
while excluding package documentation and pure product defaults. If some paths
are absent, a complete dependency set may establish that they are unused.
This check does not validate dependencies omitted by a package author.

Known gameplay roots and observed resource edges can also produce a path set,
including unavailable paths absent from the map's packages. Editor preview
branches remain excluded. Recorded graph coverage and agreement with candidate
source bytes still need verification; a known subset cannot replace full
payload coverage in the no-install check.

The runtime can inspect declaration bytes in a retained candidate over the local
library. It follows generic declaration references through the candidate's source
view, resolves entity classes from candidate parents, and reads reflected fields
and verified graph envelopes. This pass does not read native objects' prepared
state, publish the candidate, or change the observed graph. Editor branches stay
excluded. Supported expanded entity inheritance follows parent-first effective
state, including replaced fields and per-layer counted-array pruning. Unknown
readers, unsupported inheritance forms and late gameplay string consumers still
report gaps before this pass can certify a complete dependency set.

MD6 source inspection follows inherited definitions, meshes, alias animations and
every declaration an animation event argument names. An event argument is written
as its native type name and one value; when that type name is a declaration
family the frame command reader resolves it through the declaration-type
registry, so the walk carries it. An empty value names nothing. Cooked MD6 mesh
inspection follows the skeleton and per-mesh materials; cooked static model
inspection follows surface materials, including the two previous container
revisions the engine still accepts. Cooked animations name their skeleton, and
cooked skeletons, images, collision models, AAS files, rigs and opaque binary
files are verified dependency leaves rather than unread formats. These readers
use the candidate's effective source bytes. Unhandled readers retain explicit
gaps. The incomplete flag reflects recorded gaps; it is not set unconditionally
and does not turn a partial walk into complete coverage.

Audio bank catalog entries validate the filename against the cooked bank's
internal identity before native loading, in both spellings the native loader
accepts: the folded name hash and a decimal identity. Localized copies share that
name and identity, and the catalog keeps the one candidate the loader would open;
differently named banks with an identity collision are rejected. Cache filenames
never replace the engine's logical bank name. Sound event identity conversion is
implemented from the native declaration-name rules and tested against retained
prepared sound IDs. Bank indexing also records the media identities a bank names,
whether it carries their bytes, the banks its play actions name, and the
structure references it leaves unresolved. The source dependency walk connects
referenced sound names to event memberships in effective package banks, and
carries the media files those banks stream when this provider supplies them.
It retains distinct locale paths and all contributors to the selected resource,
so a bank-only replacement can select its complete supplying package. Banks
unrelated to the map's sound references do not select packages. The index reads
verified file streams or immutable cache handles and skips embedded media.
Malformed bank metadata remains an explicit dependency gap; stale bytes or
failed IO refuse the snapshot. Installed audio originals now use a separate
immutable index of PC soundbanks below the engine's own configured bank prefix,
so a configured prefix moves discovery, identity reads and bounded reads with it
and a path outside that prefix is not answered. A bank identity that exists only
inside a mounted package is indexed under the decimal file stem the native loader
uses to reopen a bank it knows only by identity; one that still cannot be read
stays counted and logged. PCK metadata supplies exact bank, media and
external-source IDs and per-package language IDs; matching packed payloads take
precedence over loose files. Only selected payload ranges are hashed, through
retained read-only handles. Equal packed candidates coalesce; differing candidates
refuse an ambiguous original rather than choosing by directory order. On this
installation the ambiguity is unreachable: five packages, 7689 entries and 161
identities present in more than one package, none with differing payloads.
Copied SnapMap originals do not gain delivery ownership. Installed campaign banks
still require gameplay activation, but their existing files satisfy availability
without an install prompt. Map-authored bytes remain authoritative and transient.
The native audio language is captured without blocking unrelated resource paths
while audio initializes; the activation catalog reads loose bytes until that
index answers and is rebuilt once it does. A bounded reader serves the effective
original bytes for that catalog without hashing a payload. Streamed media
dependency closure and loaded-bank activation and replacement are implemented as
described in `resource-composition.md`; file availability alone still does not
prove that a bank or voice has switched successfully in a running world.
Unsupported native sound-name representations
are reported by the identity reader; they do not impose a package payload limit.

The map carrier can extract a complete verified source set into a private
`package-cache/maps` directory. It checks shard digests, archive identities and
the resulting authored trees before returning a context. This directory is
outside installed-package discovery, preserves duplicate assets and empty
folders, and remains owned until the map provider and save operations retire.
An invalid set never returns a usable context. Closing a context removes its
private sources; a held file retains cleanup ownership for retry. This service
does not perform installation or decide resource availability.
Installation checks use the installed source inventory, so the map's own
private sources cannot satisfy a missing local installation.

For example, a map-authored Cyberdemon with health10 must use10 even when the
local package says12000. The local file remains12000. Live OpenGL checks verified
map-authored health12500 against unchanged local25000, then restored native25000
on browser return. This verifies the tested entity declaration path; it does not
establish replacement of every loaded model, image, shader or audio consumer.

The descriptor requires `id` and `name`. Requirements and editor strings are
inline data. For example:

```json
{
  "id": "campaign-demons",
  "name": "Campaign demons",
  "description": "Additional campaign demons for SnapMap.",
  "requirements": {
    "cvars": {
      "g_useResourceBlackList": 0,
      "g_useImageBlackList": 0
    }
  },
  "strings": {
    "en": {
      "campaign_demon_name": "Campaign demon"
    }
  }
}
```

String IDs and values use normal JSON escaping, including Unicode escapes and
surrogate pairs. The runtime decodes them to UTF-8 before native interning.
A UTF-8 byte-order mark at the start of a string document is accepted.
A malformed dictionary, non-string value or embedded NUL refuses that dictionary
before its rows are published. Explicit local string overrides still take
precedence over package text.

String bookkeeping and IDs grow with the installed data. There is no application
quota on string count, ID length or local string-document size. Native signed
32-bit string lengths and dictionary counts still apply; allocation and I/O
failures are reported. A diagnostic path cannot limit an otherwise valid key.
Local and built-in labels are available before the first package compilation;
package labels join when a compiled policy is published.

Optional `hud` contains supported presentation rules, currently
`weapons.<weapon declaration name>.ammo_display`, with `weapon` or `engine` as
the value. Requirements are supported settings, not arbitrary console commands.
Empty policy sections may be omitted. The descriptor may retain descriptive
metadata; it has no package precedence or map-inclusion switch.

Asset startup behavior belongs in the asset declarations under `assets`.
For example, a campaign boss can initialize the memory value that its combat
state expects after an intro. This travels with the package and runs through
the normal entity spawn path; it does not require another descriptor setting,
an install-time console script, or map-author wiring.

The outer package is the delivery unit. Nested descriptors identify components
inside that unit. A map using gameplay from a mixed package must carry the
whole authored package, including its editor support, strings, auxiliary files,
empty directories and duplicate components. A plain grouping folder above
packages is only organization. Files outside a component's `assets` directory
are preserved but are not served as engine resources.

Nested components are optional and useful when reusing an existing package
without rearranging its files. For example, `boss-demons` can contain the
original `cyberdemon` package alongside `hell_guard` and `spider_mastermind`:

```text
boss-demons/
  package.json
  cyberdemon/
    package.json
    assets/...
  hell_guard/
    package.json
    assets/...
  spider_mastermind/
    package.json
    assets/...
```

The outer descriptor makes this one delivered package. Its components retain
their own engine-shaped asset directories and metadata. Installing the same
Cyberdemon separately still preserves both authored copies; duplicate handling
happens in the compiled output. A package built from scratch can use the simpler
single `assets` directory shown above.

The compiler inventories complete sources, resolves exact native paths, and
compares contributions with installed originals. Equal duplicates share one
effective resource while retaining every source owner. Supported declaration
collections combine compatible entries; independent changes combine relative to
the original. Contradictory values and different opaque replacements fail with
the resource or policy field and contributing packages. There is no silent
folder-order winner.

Editor property lists combine by property path within their existing sheets.
For example, two packages adding different boolean controls to a blocking
volume retain both controls, even when both authored files use the same next
item index. The compiler rebuilds the count and indices. Identical controls
are shared; incompatible definitions of the same property report a conflict.
Existing sheet positions remain fixed because sheets have no stable identifier
for matching independently changed layouts. These rules are built into the
compiler and require no package metadata.

Reflected declaration families use the same native field metadata as entity
definitions. Their fixed arrays preserve sparse positions and bounds, while
independent object fields merge recursively. The compiler emits inheritance
before state so a newly added parent is read correctly. Materials, MD6 and
other custom grammars retain complete single replacements and exact duplicates;
different contributions require a verified format-specific composer.

All contributions participate in one merge. Compatible additions inside the
same new object or collection entry combine recursively. Collections with
verified identities retain authored insertions and ordering changes; the
compiler sorts only entries whose relative order no author specified. Conflicting
ordering constraints, including cycles across three or more packages, refuse
the compilation. A deletion conflicts with editing or moving that same entry.
Direct conflicts identify the actual contributing packages, even when other
packages supply the same resource. A cycle spanning several contributions lists
the contributors rather than assigning it to an arbitrary pair.
Collections with unknown identities or index consumers still need a verified
adapter; this does not yet cover every game collection.

When the installed catalog proves that a declaration identity is new, the
compiler can combine compatible contributions against empty original state.
Unavailable original metadata does not count as proof of absence. Authors do
not supply another baseline file or choose a merge mode.

The source inventory grows with the authored tree instead of imposing file,
component or folder-depth quotas. Directory traversal uses an explicit work
list and one open search handle. Allocation failure, unsupported Windows paths
or an unreadable entry refuse the complete scan; they never omit source files.

Descriptors and compiled declaration snapshots have no fixed byte quotas.
Declaration admission and runtime refresh tracking allocate for the complete
compiled set instead of stopping at 4,096 entries. Refresh reserves its tracking
storage before reconstructing any native resource; failed allocation leaves
those resources untouched. Native representation and allocation checks remain.

Delivery uses ZIP64 when archive counts or offsets exceed the classic ZIP
fields. This is internal transport encoding; it adds no author metadata.
Validation checks the extended records and member boundaries before extraction,
and the complete authored-tree fingerprint remains independent of ZIP encoding.

Conductor `aiTypeList` and `globalAIMods` collections combine by `aiType`,
allowing separate demon packages to extend the spawn roster. Two packages
assigning different demons to the same type report a conflict.

Opaque replacement selection compares verified lengths and SHA-256 identities.
Each physical authored contribution is checked, including identical duplicates;
a changed file cannot borrow another package's valid copy. An original reader
can supply a captured file identity without allocating its payload. Other
original readers still supply bytes, which are hashed once for that comparison.
Declarations retain their text and schema composition. Built-in and generated
resources participate in the same original-relative decision; an absent game
file does not suppress the product's original. These identities are internal,
not additional package metadata or installation requirements.

Compiled text owns its bytes. Opaque resources are copied to the application's
internal `package-cache/resources` directory under content fingerprints. Copying
and verification stream the file. Compilation seals each verified opaque cache
file with a retained read-only OS handle. Readers reopen that same protected file
identity with independent positions, 64-bit lengths and seeks. Opening a stream
neither rehashes the payload nor allocates a buffer the size of the resource.
Provider copies retain the seal, and reader streams remain valid after provider
retirement. Writes and deletion are denied until all those handles close.
These seals cover the internal cache; authored files remain editable.
Compiled declaration streams own their text, and a
source edit does not alter the active snapshot. A later successful compilation
publishes the replacement. This cache is disposable internal state, outside
authored packages and map delivery.

A failed refresh retains the entire last successful compilation: resource
bytes, source ownership and policy tables remain together. The failure is still
reported and does not admit newly installed packages. Correcting the sources
and successfully refreshing replaces the snapshot.

Removing a replacement declaration includes its verified SnapMap original in
the next native refresh, including when the last package is removed. These
owned restoration bytes carry no package ownership and do not embed anything
in a map. Re-adding an authored declaration replaces the restoration entry.
If the original cannot be read, the previous compilation remains active and
the refresh reports failure. This restoration applies to existing SnapMap
declarations. The native resident stage also handles captured cache identities;
its broader resource-family and rollback acceptance remains in progress.

At an admitted native boundary, cached-resource refresh snapshots the engine's
resource lists before parsing changes the dependency graph. Native path methods
map logical names to cooked files, such as a model's `.lwo` name to its `.bmodel`
payload. Reverse catalog lookup covers archive identities and permutations, and
recorded reverse dependencies add known consumers. Authors maintain no path map.
The recorded graph is incomplete and does not certify every native consumer.

Declaration and cached-resource reconstruction complete before pending dependency
loads begin. A declaration-owned object is reconstructed once. The stage uses
native lookup and loading, preserves the native production source mode, and holds
the renderer's synchronization/suspension scope through consumer updates. Loads
and lookups during this scope promote dependencies to survive normal map purges.
Resources whose custom source was removed use native default construction;
historical native source records cannot silently preserve the removed content.
Failure disarms remaining owned loads, restores renderer/allocator scopes and
retains affected identities for recovery against the previous provider, even if
failed parsing changed graph edges. No restart or game reset is part of this path.

Both executable binding checks and focused failure/order tests cover this stage.
Live OpenGL adapter checks cover exact native path selection, static model reread,
retirement to a native default and restoration, with the process left running.
Different authored binary replacements, other cached resource families and full
map-entry recovery still require live acceptance before release.

Internal declaration aliases follow the current compiled provider for both file
reads and existence probes. A failed first activation with no previous provider,
or a removed new declaration, cannot serve historical publication bytes. An
already-open stream retains its own bytes through failure or replacement; a
later open uses the current compilation. Vanilla restoration entries remain
available without assigning package ownership.

Palette rebuilds construct anonymous entity defaults for the catalog. An empty
Universal Traversal default does not enter the native decoder for adding a
traversal to a map. Placed entities still use the normal native validation.

Map policy selection uses the same typed resource ownership as package delivery.
It includes the requirements, strings and HUD rules of every component inside
each selected outer package. An editor-only reference does not select a package;
a gameplay use of a mixed package includes its editor support. No additional
descriptor flag or map variable controls this decision.

HUD rules are activated when a map loads, is saved, or starts Play. Play reads
the current native map snapshot before world allocation, so unsaved additions
and removals update the selection. This read does not save the map, embed a
payload, or consume a pending rawmap save. A failed activation refuses Play
before deactivating the editor, with an in-game error notice. A map
without contributing gameplay packages has no package HUD rules. Opening another
map clears the previous selection before parsing, and a package refresh resolves
the current map again against the new inventory instead of reusing folder-order
indices. Invalid map selection cannot publish a partially selected policy.
Editor localization continues to use the installed strings so all installed
authoring tools remain available. Resource registration also uses the installed
requirements while preparing those resources. It retains each cvar's displaced
value across rescans and restores it when its last requirement is removed,
preserving a later external edit. Application and restoration verify native
readback; a failed batch restores its prior values before a retry can proceed.
The supported blacklist requirements are resource loading permissions. They
remain active across map changes while the requesting packages are installed:
the engine can instantiate prepared editor resources during a vanilla map load
and checks the blacklist before looking up an already loaded resource. Closing
that permission at Play can invalidate the shared native registry. This does
not select an unused package for embedding or add its gameplay HUD policy.
Selected packages still carry their own requirements to the receiving client,
where installation applies them before publishing resources. No author switch
or extra manifest field is needed. Complete late dependency coverage remains
under implementation.

Maps carry compressed archives of complete authored packages. Fingerprints
identify exact content and let installation distinguish an already-installed
package from a changed package with the same ID. They are calculated by the
product. One consent decision covers the map's missing package set. Installation
validates every archive, extracts the missing packages under one temporary
grouping folder, and verifies their complete source identities. It then moves
that group into `overrides` in one same-volume rename. For example, a downloaded
set may appear as `overrides/map-<identifier>/campaign-demons/package.json`.
The group has no descriptor of its own: each package remains an independent,
unchanged authored tree. The identifier is internal and gives no precedence.
Authors can organize installed packages in ordinary grouping folders.

The rename publishes the complete set for compilation. Installation commits
only after native activation and its allocator scope finish successfully.
A private pending record stays under `package-staging` until that point, outside
authored packages. If activation fails, the previous provider is restored and
the entire new group moves back out of `overrides`. If the process stops before
commit, startup cancels that group before compiling or admitting package maps.
The player loads the map again to install from the beginning; interrupted
installations are never resumed. Failure to move a group back retains its
cancellation record and refuses admission. Temporary-file cleanup occurs only
after the complete group is outside `overrides`.

Choosing No cancels the current load's installation. A later map load can ask
again in the same game session; idle polling never repeats a declined prompt.
An activation failure directs the player to resolve the logged error before
retrying. In particular, retrying cannot resolve two packages assigning different
values to the same setting. A blocked startup cancellation also blocks manual
refresh; after the filesystem issue is fixed, refresh retries recovery before
compiling any package sources.

Installation attempts for the same data root remain serialized across processes
through activation. No receipt files are added to authored folders. Existing author folders are
preserved. A complete identical package added under another folder name while
the prompt is open is reused. Different content with the same package ID is
reported instead of overwriting it. Compiler or native registration failures
still prevent loading a map that requires the unregistered resources.
An authored package edited and refreshed during the session is recognized from
the current compilation and its complete on-disk source identity. It does not
need another installation prompt or a restart. Matching files only pass the map
gate once compilation and native registration succeed; edits made after that
compilation invalidate the match.
Session installation records grow as needed, without a separate 32-entry limit.
The native prompt keeps the installation question visible and lists the package
names that fit, followed by the number remaining.

The former small total-payload caps are removed. Transport still respects native
signed string and resource-file lengths; archive counts and offsets use ZIP64
when needed. The carrier uses 8 KiB chunks
because larger individual strings failed native deserialization; more data uses
more chunks. This is separate from the total payload size. Available memory and
disk can still cause an operation to fail with a diagnostic.
DOOM's native Save As builder and its cloud saved/published file reader each had a
separate 10 MiB JSON ceiling. Verified code patches admit positive signed idStr
lengths on both renderers while preserving rejection of zero and negative
lengths. Both sites are checked before patching; a failed write rolls the pair
back. The reader's shared size constant is unchanged.
Rawmap import uses that same native length representation instead of a 64 MiB
file ceiling. These changes require no package metadata or author setting.
The dynamic declaration provider also allocates its publication table as needed;
it has no fixed 512-entry ceiling. Refresh preserves existing open streams.
Declaration ordering recognizes quoted resource names and material table
expressions such as `blink[time]`, so packaged tables load before their materials.
Runtime declaration refresh runs on the main thread at the settled browser.
It releases permanent lifetime only during each synchronous native
reconstruction, restoring it before parsing and on exception. All targets are
reconstructed before any dependent declaration can reload.

The resource registry binds 351 declaration types and 24 other native resource
managers, independently checked in both renderer binaries. That registry is not
a complete dependency graph. Map selection follows exact typed roots and known
dependency edges to declaration identities and native provider paths. This
includes packages replacing dependencies of vanilla entities, and retains all
owners of identical duplicates. It excludes known editor declarations and
their dependency subtrees, entity changes confined to `editorVars`,
and references nested anywhere inside a saved `editorVars` subtree. Editor
previews do not trigger delivery of gameplay packages they happen to reference.
The save diagnostic reports whether all visited source/state phases were
observed or some remain partial; this does not prove later string consumers.
Missing observations retain known owners; they neither prove absence of further
dependencies nor force vanilla maps to embed every installed package. Invalid
map data or an interrupted graph walk refuses selection instead of publishing
a partially traversed result.
Dependency capture records declaration parsing and canonical entity-state loading
separately. Source reload invalidates the old state dependencies, and a stale
in-flight result cannot complete the new source. Inline map edits and partial
state updates cannot overwrite a shared declaration's dependency records.
Inline `entityDef.state.edit` fields are also inspected through native reflection:
the engine can serialize a resource pointer as an ordinary JSON string, such as
Give Item's inventory declaration. The field's registered reader determines
whether that string is a resource. These references remain local to the map and
feed both whole-package delivery and active policy selection. Runtime copies
their identities and resolves ownership again after inventory refresh. An absent
inline class uses the inherited declaration's prepared class. Editor preview
subtrees, null references and cleared fields do not add gameplay roots; unknown
custom readers remain coverage gaps. No author-provided file mapping is required.
Direct declaration reparsing participates in the same invalidation, including
paths used by editing and native `touchDecl`. Temporary declarations without a
registered resource identity isolate their references from surrounding parses.
The native production-file reader is also captured because it can call the
typed parser directly. An implicit declaration's successful no-op read does
not replace its dependencies or turn an unobserved source into a complete one.
Loads that happened before capture started remain explicitly unobserved.
Startup resolves the observer's signatures first and installs it before scanning
the remaining features. Those verified addresses remain in the bootstrap's
pre-install snapshot, so later binding does not mistake its own detours for
missing engine functions. Smoke diagnostics reuse that snapshot.
An entity definition with no observed state load is reported as incomplete.
When the engine expands inherited state into the child, that child's state
capture supplies its effective fields; inheritance edges still visit parent
sources without requiring unrelated standalone parent instances. Unexpanded
state and actual entity references retain the parent's state requirement.
Declaration composition and static dependency inspection share an assignment
syntax tree. Parsing and cleanup use heap storage, with no fixed text-size,
node-count or nesting cap. Composition still has a structural depth bound.
The typed walker visits reflected fields through compiler-owned adapters for
native resource readers and collections. `decl_native_schema` reads the native
class and enum registries through a guarded memory-reader boundary, copies
identities, and reads inherited fields on demand. Registered callback identities
select supported readers; the adapter never invokes those callbacks. Native
array dimensions and collection element metadata determine traversal. Conflicting
collection registrations, missing field metadata and unsupported custom readers
remain explicit gaps. It excludes top-level `editorVars`,
does not construct game objects, and reports unsupported semantics as gaps while
continuing through known siblings. Package authors supply no dependency schema.
For a prospective map, supported expanded entity definitions are resolved from
their retained source chain before inspecting their edit fields. Child fields
replace parent fields, ordinary blocks retain untouched inherited fields, reset
markers remain in the expanded text, and each layer's list count removes excess
inherited items. Parent declaration files remain dependencies, but an overridden
parent value no longer creates an unrelated gameplay reference. Explicitly
unexpanded state, unsupported grammar or field shapes still report coverage gaps.

An own-state walk does not establish inherited or transitive closure. Already
parsed entity definitions retain native prepared state with inherited fields,
available before an entity is spawned. Before selecting packages, a main-thread
pass reads prepared state for map entity roots and fills missing graph records.
It preserves complete native captures and previously known edges when a static
read fails. This pass finishes before acquiring the package snapshot because
native source probes may enter the resource provider. Typed deserialization does
not establish the dependencies of later gameplay consumers of string data.

The generic declaration-reference resolver reads native manager registration
order, class names and parent links. It checks a matching manager's loaded
resources, then loaded descendants, then that manager's source and descendant
sources. When no class matches, it repeats this lookup in registration order.
This distinguishes, for example, weapon and ammo resources referenced through
`idDeclInventory`; a fixed class-to-folder mapping cannot express that lookup.
Source availability must describe the engine's active source mode. Development
source records alone are insufficient when the engine uses production archives.
Unknown availability stops resolution instead of choosing a later resource.
Prospective map-source analysis uses a separate resolver over the same manager
metadata. It checks only the candidate's source view; resident objects from a
different local composition cannot choose a family or establish that a candidate
source exists. This does not predict how stale native objects would behave before
activation; native refresh remains a separate requirement.

The adapter never creates defaults or loads game objects. Explicit unloaded
`.decl` paths and non-ASCII name normalization remain unsupported. Image and
render-model readers require separate resolution. Native source selection is
read from a verified binding; production existence uses a quiet metadata probe
and includes published package declarations without loading game objects.

Installation inventory stays private through resource activation and native map
loading. The owning map request commits the whole disk installation only after
that load succeeds, then publishes the prepared inventory without another scan
or allocation. Failed, canceled or superseded loads retain the previous installed
inventory. Saved-map continuation also requires the retained JSON to have reached
the native parser; an early zero return cannot commit an unused installation.

Transitive dependencies, map-specific activation, atomic
multi-package installation and native consumer refresh remain under validation.
These must be completed before releasing the compiler. Vanilla Unknown,
Timeline and Lift authoring must continue to save without mod delivery when no
gameplay replacement is used.

## Built-in editor controls

For declarations with a verified installed original, the compiler also composes
SnapMap Plus's built-in defaults with package contributions. The production
compiler receives built-ins and intact authored sources together, including
built-in identities that no package overrides. There is no separate per-resource
built-in recomposition pass. Adding blocking-volume
properties therefore retains the built-in navigation control. Removing the packages
restores the built-in control alongside the original game properties. Built-in data
adds no package owner and is never embedded as a package dependency. Contradictory
edits are reported before replacing the active compilation. For a built-in identity
absent from the game archives, the product's own declaration supplies the baseline.
Copies of those declarations therefore compose and restore through the same path.
