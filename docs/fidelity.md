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
Map rendering defaults to the game's original module environments. View Distance
8192 (or zero) and Fog Strength zero restore those environments automatically;
unused fog range/color values do not keep an override active. The seven saved
values occupy an ordinary map variable that vanilla can load, with no separate
enable or compatibility flag. Legacy saved values remain readable; the former
disabled mode migrates to defaults. Zero fog now preserves built-in module fog.

Embedded packages are selected from the map's current declaration references
on each native save. Deleting the last dependent object removes its package
payload from that map; packages still referenced by objects or logic remain,
and installed client packages are not removed. A native save/reopen test with a
resized room, custom rendering and Cyberdemon retained the identical embedded
package. After deleting Cyberdemon and resetting the room and rendering,
the same saved map loaded and completed Play/return with both Snapmap+ DLLs
uninstalled on Vulkan. This does not establish compatibility for unrelated
mod-only content or console platforms.

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
