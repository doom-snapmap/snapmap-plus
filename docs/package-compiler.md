# Package compiler

A package is an authored folder with one `package.json` and an `assets/`
directory at the exact paths the engine requests. SnapMap Plus discovers and
compiles those files, carries complete selected packages in maps, and installs
complete bundles only when the client lacks required resources.

## Authoring a package

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

Keep each extractor's actual path, extension and permutation directories.
A `.bmodel` in `cooked/model`, a compiled MD6 mesh in
`generated/basemodel`, and a definition in `generated/decls/md6def` serve
different engine requests. Do not flatten them into a common models folder.

The descriptor requires `id` and `name`. For example:

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

Requirements, strings and supported presentation policy belong in this file.
Optional `hud.weapons.<weapon declaration name>.ammo_display` accepts
`weapon` or `engine`. Requirements are supported settings, not arbitrary
console scripts. Empty sections may be omitted.

There are no version fields, legacy descriptor reader, file lists, path mappings,
author hashes, public receipts, precedence switches or map-inclusion switches.
Other authored files, such as documentation, are preserved but are not served as
engine resources. Compiled output and installation bookkeeping live in application
data outside the authored folders.

Boss startup behavior belongs in its asset declarations. An intro-complete
memory value can therefore travel with the boss and initialize through ordinary
spawning, without install-time commands or extra map wiring.

A package may contain optional nested components, each with its own descriptor
and `assets` directory. An outer `boss-demons/package.json` makes Cyberdemon,
Hell Guard and Spider Mastermind components one delivery unit. A plain folder
with no descriptor only groups independent packages. A new package usually needs
only the single assets directory shown above.

## Compilation and duplicate resources

Compilation inventories authored sources and verified installed originals. Built-in
product overrides participate through the same resource model. Source files are
never edited by compilation. See [Resource composition](resource-composition.md)
for the native reader contracts and collection rules.

Equal contributions share effective bytes while retaining every owner. Independent
changes compose relative to the original: two packages adding different blocking
volume booleans retain both, including when both used the same next serialized
index. Reflected fields, named collections, ordered graph entries, MD6 records,
material writes and supported custom declaration blocks use their native semantics.
Counts, order and references are rebuilt and validated.

Two incompatible changes to the same value, contradictory ordering, or deleting
an entry another package edits produce a diagnostic naming the resource and
contributors. There is no folder-order winner. A valid standalone binary or program
payload is selected whole. Arbitrarily concatenating models, images, compiled
shaders or audio banks would corrupt their native format.

All overrides are served through the runtime provider. Installed game archives,
including `snapgameresources`, are never overwritten or repacked. Whole-payload
selection does not mean replacing an installed game file.

## Which packages travel with a map

Gameplay resource use selects package owners automatically. A package that changes
an existing gameplay resource can be selected just like one that introduces a new
resource. The selected outer package travels intact, including duplicate copies,
nested components, strings, requirements, editor helpers, auxiliary files and
empty directories.

Editor-only usage does not select a package. A local tool that exposes a native
Unknown, idTarget command, Timeline or Lift does not turn that native entity into
a mod. Resources already supplied unchanged by vanilla SnapMap do not gain
delivery ownership. A map using only such stock content has no package carrier
and remains loadable by vanilla players.

A mixed editor/gameplay package is an intentional authored unit. Once gameplay
uses it, its editor support and strings travel too. No author flags are needed
to distinguish these cases.

Dependency discovery combines native field metadata and verified readers for
graphs, MD6, cooked models/animations, materials and audio. Strings are interpreted
as resource references only when an actual native consumer establishes that
meaning. Full payload availability provides a conservative no-install check;
a complete dependency set may show that a missing supplied path is unused.
Malformed or unresolved inputs produce explicit diagnostics.

## Local library versus map values

The local library and the active map are separate compiled views. Map-authored
resources and matching policy are authoritative in both Play and Edit. Other
local authoring resources remain available. Leaving the map restores the local
view without changing authored files.

If the local Cyberdemon has 12,000 health and the map carries 10, the map uses 10.
The local package still contains 12,000 afterward. This rule applies to resource
paths and policy generally, including opaque payloads; it is not a health-field
exception. Packages inside one map still compose together and diagnose their own
contradictory edits. A different local value is not such a map conflict.

A saved or copied map retains its incoming authored package provenance even when
installation was skipped. Editing that map does not silently substitute the local
package's different contents. After an installation, the newly available resources
are also usable to author another map.

## Installation decisions

Availability is checked against the installed resource inventory and verified
SnapMap/product originals. The map's private extracted bundle cannot count as an
already-installed library resource. Package IDs, folder names, bundle shape and
different local values do not themselves cause an installation prompt.

| Client state | Result |
| --- | --- |
| All required resources already available | No install prompt. Activate the map's authored view and load. |
| Cyberdemon installed; map carries a differently grouped Cyberdemon | No prompt if its required paths are available. Preserve the map's whole authored bundle on resave. |
| Cyberdemon installed; Boss Demons map also needs missing Hell Guard or Spider resources | Prompt for the complete supplying Boss Demons bundle, including its duplicate Cyberdemon files. |
| Same package ID locally, but incoming bundle supplies a missing required path | Install the complete incoming variant beside the unchanged authored local folder. |
| Two incoming packages supply a missing resource | Keep their complete delivery ownership; do not punch duplicate-file holes in either package. |
| Only resource values differ | No installation solely for that difference; map values govern transiently. |
| Required resource is absent and no bundle supplies it | Report the missing resource; do not silently load with a default substitute. |
| Stock SnapMap-only map | No mod delivery or installation. |
| Player declines | Cancel that pending mod-dependent load; keep the current installation intact. Retrying the map can ask again. |

Delivered variants can fill missing library paths without overwriting the local
authored variant's overlapping values. On the receiving machine, installed
packages remain complete authorable bundles. Another map can embed them again,
continuing the author-play-author chain.

Installation uses one native in-game question for the missing package set.
It validates complete archives and trees in a private staging directory, publishes
one complete grouping directory beneath `overrides`, activates the retained map
candidate, and continues the original request with its retained JSON. It commits
only after activation and native map loading succeed.

For example:

```text
overrides/
  my-authored-cyberdemon/
    package.json
    assets/...
  map-<internal identifier>/
    local.boss-demons/
      package.json
      ...
    local.cyberdemon/
      package.json
      assets/...
```

The grouping identifier confers no priority and has no descriptor. Users can
organize packages in ordinary grouping folders. Fingerprints protect content
integrity and recognize exact trees; they are computed internally and do not
decide whether different local values require an install.

## Cancellation, activation and lifetime

Cancellation, interruption, supersession or activation failure cancels the entire
new install group. There is no partial install to resume. After a process
interruption, startup cancels an uncommitted group before package admission.
The player retries installation from the beginning. A failed filesystem rollback
keeps its recovery record and blocks admission until recovery succeeds.

Provider, policy and installed inventory publish transactionally. A partial native
activation recovers against the previous provider before releasing failed sources.
New native identities without a previous source return to their engine default;
their failed installed files are removed. Existing open streams retain their
original immutable bytes until they close.

Native declaration and cached-resource reconstruction, renderer synchronization,
persistent allocator ownership, palette rebuilding and audio-bank reconciliation
occur at the established engine lifecycle boundary. A committed install marks
the local library for recompilation before a temporary map view retires. If that
rescan fails, the current provider stays owned rather than disappearing.

Installing, serving, restoring or canceling packages does not restart/reset DOOM
or force a map reload as a recovery mechanism. Normal Play/Edit/map transitions
still perform the game's own level lifecycle.

Installed strings remain available to editor tools. Supported blacklist
requirements are resource-loading permissions and stay active while their
requesting packages are installed; the editor can instantiate those resources
even when a vanilla map is selected. This does not embed those packages or enable
unused gameplay HUD policy. Removing the final requirement restores the displaced
native value while preserving a later external edit.

## Transport and validation

Maps carry compressed complete authored trees in native string shards. ZIP64
handles large archive counts/offsets, and the former small total-payload caps are
removed. Individual native strings, signed native lengths, allocations and I/O
still have their real engine/system bounds; chunking avoids the measured native
single-string limit. Integrity checks do not alter the author's tree.

One retained map-data admission/activation pipeline is shared by existing backend
entry adapters. Published-map and multiplayer route testing are outside this
acceptance scope; no separate route policies or future route-test tasks are
defined here.

Offline compiler/installer/runtime suites and both executable binding gates have
passed. Saved-map installation, decline/retry, interruption, whole-tree fidelity,
no-prompt reuse, transient values and repeated Play/Edit restoration have live
OpenGL and Vulkan evidence. Controlled failure after publishing a new install
group and same-ID missing-resource installation have additional Vulkan evidence.
The stock Unknown/idTarget/Timeline/Lift fixture was loaded with the product
uninstalled. Current-candidate boss combat acceptance is recorded separately in
the development campaign.


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
