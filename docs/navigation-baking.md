# How Blocking Box navigation is baked

Snapmap+ extends each module instance's shipped AAS with surfaces from its
marked Blocking Boxes. [Navigation](navigation.md) describes the editor workflow,
preview, ownership and supported geometry; [navigation markers](navigation-markers.md)
explains the saved opt-in and runtime obstacle policy.

The baker examines all six transformed faces and selects exposed faces whose
normals satisfy the AAS class's slope limit relative to world up. “Top” is
therefore determined after rotation: a box's local Z axis may face sideways or
down. Vertical walls and ceilings do not become grounded walk surfaces simply
because a box is marked.

Intersecting solids subtract buried surfaces. Overlapping walkable coverage is
partitioned into convex pieces, with clearance applied at exposed boundaries.
A construction can remain several connected AAS areas; it does not need to
become a single polygon. Each monster size has its own clearance and movement
settings. The green preview displays the monster48 result, not every class's
usable area or reachable route.

## Contacts with bridges and module floors

A walk link needs physical floor contact as well as graph connectivity. The
baker preserves support across contacts within the class's step height, splits
partially shared edges, and leaves the exposed portions marked as walls. It
checks integer-quantized walk endpoints and rejects gaps and wall crossings.
A lower bridge can be a step; an arbitrarily high ledge requires an appropriate
native traversal capability.

Existing module floor polygons also participate in support calculations. This
prevents clearance erosion from pulling a ramp's exit back from a floor that
already supports the agent. These polygons are neither emitted again nor
subtracted as occupied solids. Native sloped floors use the plane height at the
contact position rather than the maximum Z of their bounding box. Contacts
crossing the step-height boundary are split so the whole edge is not assigned
one movement classification.

## Visibility is part of movement data

Native obstacle and wall selection decode an area's `obstaclePVS` row against
the current area count. The corridor consumer truncates its area sequence at
an area absent from that visibility set. Copying an old carrier's compressed
row can therefore leave a bridge's new areas invisible even when reciprocal
walk links exist.

After adding areas and links, the baker rebuilds complete visibility rows for
the final area count. It expands original visibility through each generated
area's carrier and includes actual new route neighbors in both directions.
This extends visibility where the construction crosses an original partition;
it does not mark every area visible to every other area. Existing and generated
areas receive complete rows in the native seven-bit-literal/zero-run encoding.

## Coverage and remaining limits

Automated coverage includes rotated ramp-to-floor contacts at several agent
radii, connected bridge graphs, compressed visibility decoding and rebuilt rows,
quantized endpoints, native slopes, and renderer-specific patch discovery.
These checks do not establish that every demon traverses every construction in
live play. Animation choice, dynamic obstructions and other native movement
constraints still matter.

Geometry remains bounded: 32 corners per convex piece, 512 generated pieces per
module bake, the native limit of 256 outgoing links per area, coordinate limits,
and the native BSP depth cap of 128. Native floor support extraction also
requires a valid convex polygon of at most 32 corners; more complex native
areas do not gain this support treatment. Invalid or over-capacity candidates
are refused where validated, preserving shipped navigation. The system does
not promise arbitrary mesh support, runtime moving platforms, changing gravity,
or unlimited construction complexity.
