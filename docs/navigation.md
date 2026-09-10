# Blocking Box navigation

Enable **AI Navigation** and **Block Demons** on each Blocking Box that should
support grounded demons. Snapmap+ builds navigation from their current placement,
rotation and dimensions. An unmarked box that blocks demons still obstructs
navigation on a marked box.

## Surfaces and traversal

The bake considers all six faces after rotation, then retains exposed faces that
meet the monster class's slope limit. A box on its side can therefore contribute
its former side face, and a tilted box can contribute two faces. Vertical walls
and downward-facing ceilings are collision boundaries, not ordinary walk areas.
Climbing and jumping require separate links supported by the demon's shipped
traversal animations.

Intersecting solids remove buried surfaces. Coplanar overlaps are partitioned
without duplicate coverage, and agent clearance is applied to the outside of the
support union rather than independently to each box. This preserves a continuous
bridge deck and avoids artificial seams between flush boxes. Walkable ramp
contacts retain their true floor height. Obstacles and overhead volumes use their
actual oriented shape and underside; a floating deck is not treated as solid all
the way down to the module floor.

The AAS remains a set of convex areas connected by reachabilities. A connected
construction does not have to become one area, and a valid isolated platform
does not automatically have a route up from the module floor. Different monster
sizes can have different usable space and traversal links.

## Editing and preview

While the editor is active, Snapmap+ takes a complete geometry snapshot roughly
once a second on the game thread. It compares geometry and module ownership,
including new IDs created by copy/paste. Changed snapshots invalidate the old
bake; unchanged snapshots reuse it. Deleting the last marked box removes custom
navigation. An incomplete snapshot makes custom navigation unavailable until a
complete read succeeds.

Green lines show the generated walk areas from the validated **monster48** bake.
The native renderer draws them on the construction in the module's Object Mode;
Blueprint Mode does not display them. This overlay is separate from a module's
own grid material and does not require enabling the engine's debug render modes.
They include area outlines and a grid, with the monster's clearance already
removed. They do not indicate reachability from the player or suitability for
larger demons. `navmesh.preview` controls this display and defaults to true.
`sh_navmesh` reports the current geometry revision, per-instance results and any
refusals. Large previews have an 8,192-line display limit.

The preview follows geometry once the editor exposes the changed placement in
its map state, usually within one refresh interval. This is periodic rebuilding,
not an instantaneous update at every mouse movement. Before Play builds its AAS,
Snapmap+ takes a final snapshot and freezes that revision for the build. This
prevents a class or module from consuming half of a newer edit.

## Module ownership

Each placed module has its own bake. Ownership comes from the editor's
`instanceEntities` table, not the nearest origin or a module name. Two Grid Rooms
can contain completely different structures, including at identical local
coordinates. During the engine's build, private temporary resource names select
the exact instance and geometry revision. The engine then applies that instance's
transform and merges it normally.
Incomplete or ambiguous ownership and non-finite transforms refuse the snapshot.
Orphan entities are excluded because they have no module owner.

This does not invent cross-module routes. Connections through module portals
remain the engine's responsibility. The older `smnav1` embedded-payload format
retains its existing repeated-module restrictions; it is separate from baking
the current Blocking Boxes.

## Boundaries

This extends the module's shipped AAS; it is not a replacement compiler for all
rendered or collision geometry. It supports oriented Blocking Boxes within the
module's existing AAS space. Non-box primitives, arbitrary meshes, runtime moving
platforms, changing gravity and wall-walking need additional geometry or movement
support. Module architecture and traversal animations still constrain movement.

The implementation uses bounded convex clipping and floating-point tolerances.
Invalid input or exhausted geometry capacity refuses the candidate rather than
publishing a truncated surface set. Limits include 512 relevant boxes per map,
512 generated convex pieces per module bake, 32 corners per piece, the native
AAS coordinate range, and the traversal-record capacity. Refusal retains the
module's shipped navigation; it does not retain a stale custom bake.
Complex layouts still need Play testing,
especially narrow transitions, ceilings and animated traversals. A green surface
proves that the bake produced an area, not that every demon can reach it.
