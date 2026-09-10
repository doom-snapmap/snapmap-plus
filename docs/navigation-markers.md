# Blocking Box navigation markers

The native SnapMap **AI Navigation** property now writes the existing
`idEntity.flags.noFlood` boolean on Blocking Boxes. The baker reads only that
nested field. The property is available in the in-module entity settings;
`navmesh.preview` controls the green editor preview separately. Block Demons
must also be enabled for the volume to physically support a demon.

The former marker, `affectsNavmesh`, has a native effect in both the Vulkan and
OpenGL executables: blocking-volume setup removes `CONTENTS_OBSTACLE` from its
collision contents when the flag is true. It does not remove `CONTENTS_MONSTERCLIP`.
The earlier source comment claiming that nothing consumed the field was wrong;
string references alone missed the compiled member access.

`flags.noFlood` already has boolean reflection, serialization, and defaults in
both supported executables. The Blocking Box declaration does not override its
false default. Static examination of the field's bit accesses found no gameplay
consumer. This is a finding about the supported retail builds, not a promise
about every engine build or external mod. We add no unknown boolean, class, or
inheritance path. A new custom field was not selected because its vanilla
loading behavior has not been established.

## Existing maps

Before native map parsing, Snapmap+ migrates Blocking Boxes with
`affectsNavmesh: true`:

- If `flags.noFlood` is absent, it is added as `true`.
- An existing `flags.noFlood` value wins, including `false`.
- `affectsNavmesh` is set to `false`, restoring the native collision-content
  behavior to the unmarked-box setting.

Other entity types, other entity flags, geometry, module ownership, and map
variables are preserved. Migration occurs in memory for ordinary map loads
and rawmap loads, independently of the rawmaps setting. Saving records the
migrated state; opening a map does not rewrite its source file. Reloading an
already migrated map makes no further change. Malformed state or an allocation
failure leaves the original input intact rather than partially converting it.

Newly placed and copied boxes use the new property directly. The native
`affectsNavmeshPath` binding remains pointed at the original field, separate
from the added property row. This avoids redirecting a native editor behavior
into the custom marker. Old product versions do not recognize the new marker;
edit migrated maps with a current version. Vanilla clients know the field and
continue to use the module's normal navigation rather than our custom bake.

## Validation and limits

Offline regressions cover the new marker in map reads, editor refreshes,
complete snapshots, and baking, plus migration with missing, empty, and
populated flag objects, explicit on/off precedence, idempotence, refusal, and
the shipped property binding. No live game or daemon test was performed for
this change.

The switch isolates the bake opt-in from a native collision flag. It does not
prove a fix for demons becoming stuck on a platform. `blockDemons` also drives
a separate native blocked-volume registration, and traversal still depends on
the generated AAS connections, clearance, and the demon's movement behavior.
