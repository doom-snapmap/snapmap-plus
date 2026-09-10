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
false default. A bounded static examination of direct accesses to this flag
found no gameplay consumer; the broader overlapping-memory scan was not a
complete audit. This is a finding about the supported retail builds, not a
promise about every engine build or external mod. We add no unknown boolean, class, or
inheritance path. A new custom field was not selected because its vanilla
loading behavior has not been established.

## Existing maps

Before native map parsing, Snapmap+ migrates Blocking Boxes with
`affectsNavmesh: true`:

- If `flags.noFlood` is absent, it is added as `true`.
- An existing `flags.noFlood` value wins, including `false`.
- `affectsNavmesh` is set to `false` in the serialized state. Runtime obstacle
  handling is derived separately as described below.

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

## Runtime obstacle handling

Separating the marker did not make the original collision flag harmless.
With Block Demons enabled, setting `affectsNavmesh` to false reintroduces
`CONTENTS_OBSTACLE`. Native clip-model contents updates use that bit to register
or unregister an obstacle in AAS; the movement corridor gathers those obstacles.
The earlier migration changed this behavior as well as moving the opt-in.

Snapmap+ now extends the native contents-update condition. It removes
`CONTENTS_OBSTACLE` for a Blocking Box when all three conditions hold:

- `flags.noFlood` is true.
- Block Demons is enabled.
- `navmesh.enabled` is enabled.

An existing true `affectsNavmesh` retains its original native effect, regardless
of the custom marker or configuration. The patch changes the runtime condition,
not the saved flags. Already migrated maps therefore receive the policy when
their boxes undergo native contents setup, without manually toggling or
recreating them. Spawned and copied boxes use that same native setup path.

Physical demon collision remains enabled. Block Demons also registers a
separate expanded blocked volume; this patch does not remove that mechanism.
The condition is evaluated during native contents updates, so changing the
configuration alone is not a guarantee of immediate reclassification of every
already active volume. The supported Vulkan and OpenGL builds have verified
patch sites. If installation fails, the backend reports
`NAV: marked-volume obstacle policy unavailable`.

## Validation and limits

Offline regressions cover the new marker in map reads, editor refreshes,
complete snapshots, and baking, plus migration with missing, empty, and
populated flag objects, explicit on/off precedence, idempotence, refusal, and
the shipped property binding. Native evidence verifies the obstacle-bit
consumer on both supported renderers. These checks establish storage and
implementation behavior; they do not establish a live movement result for every
demon class.

The marker and runtime contents policy address different concerns. Neither
alone proves that a demon can cross a bridge or leave a platform. Traversal also
depends on surface contact, AAS connections, obstacle visibility, clearance and
the demon's movement behavior. See [baking details](navigation-baking.md).
