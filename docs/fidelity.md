# SnapHak compatibility

Snapmap+ preserves useful SnapHak workflows while maintaining its own frontend,
engine integration and fault handling. This reference records compatibility
choices contributors should retain when changing those paths.

## Preserved workflows

- **Frontend loop:** the companion window has its own roughly 30 Hz worker
  loop. Requests captured by WebView callbacks are issued from that loop;
  declaration commits use the backend's main-thread transport.
- **Save to Decl:** there is no complete class/inherit compatibility check.
  Snapmap+ rejects known incompatible combinations, but that narrow guard does
  not prove every accepted edit is valid.
- **Create from selection:** the engine requires hovering an entity in the
  selection. The frontend checks the hovered ID and reports the requirement.
- **Prefab staging:** a loaded prefab remains available for native Ctrl+V.
  Load / Place also requests the engine's paste action when editor state
  permits it. Staging uses a heap that survives map transitions.

## Intentional differences

Grid Room resizing is included in the backend and uses DOOM's native Module
Properties panel and Blueprint X action. Its saved module identities require
runtime resources reconstructed by Snapmap+. Such maps are not compatible with
vanilla or console players, even when their placed entities use stock assets.
Restoring the original dimensions restores the stock module declaration.
Map rendering defaults to the game's original module environments, and with
Unlock View Distance off the map carries no Snapmap+ rendering data: turning
the setting off removes its variable, so the saved map is byte for byte what
the unmodified game writes. On draws to 200000 with every environment's
fog kept and stretched by the same factor, in the editor and in Play; it occupies one ordinary map string
variable that a vanilla client loads and ignores, keeping native distance and
fog. Older saved View Distance values remain readable: 8192 reads as off and
is removed, any other distance reads as on; their fog values are ignored and
the former disabled mode reads as off. Fog is not a map setting.

Entity placement is unrestricted. The unmodified editor refuses a held demon,
player start, encounter spawner, AI point, item hint or pickup unless a floor
is under it, and shows it red; with Snapmap+ any entity can be placed or moved
into open air, while a floor or wall under it still counts as before. Overlap
and outside-every-module refusals are unchanged. The map stores only the
entity's position, so it loads in the unmodified game, where a floating entity
stays where it was placed but turns red and cannot be put down in the air again
once it is picked up. In Play the player starts on the floor under a floating
player start, and floating demons are on the floor once they are active.

A Snapmap+ map carries no mod files: it carries one `smpmod1.` string
variable per installed mod it uses, the files stay in
`%LOCALAPPDATA%\snapmap-plus\mods\`, and the unmodified game reads the rows as
inert strings. A map carrying packages from an earlier Snapmap+ release has a
complete package converted into a local mod on load and any other package
removed. See [Mods](capabilities.md#mods).

The `sh` console dispatcher executes SnapStack handlers on DOOM's main thread,
inside the engine's command callback. SnapHak queued these handlers onto its
UI thread because they touched Qt objects. Snapmap+'s handlers do not require
that thread. The interface queue remains in the ABI, and its drain still runs
the backend tick. See [threading](architecture.md#threads-and-edit-results).

`bsb` reports failed property round trips with a non-modal toast. `filtcls`
reports the class field it filtered, rather than reusing the inherit label.

The backend includes a fault shield for selected engine errors and exceptions.
Recoverable cases can return control to the editor or menu; fatal cases retain
crash evidence. Recovery is bounded and does not make arbitrary invalid edits
safe. See [`src/fault_shield/`](../src/fault_shield/).

## Unsupported SnapHak features

- **Lua scripting:** no Lua runtime or empty Lua tab is shipped.
- **Render-model count overlay:** `sh_show_rmcount` is not registered.
  `sh_debugrender dumprenderinfo` prints the count and model names. The native
  navigation line renderer does not implement this text overlay.
- **Dash and meathook tuning:** the six original `cs_dash_*` and `cs_mh_*`
  movement cvars are not registered because their movement implementation is
  absent. Add the behavior and settings together if this feature is ported.

The [feature inventory](capabilities.md) describes the supported surface.
Detailed research and abandoned designs are retained in snaphak-re's findings
system; product documentation describes maintained behavior.
