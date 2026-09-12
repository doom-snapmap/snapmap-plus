# Changelog

Every Snapmap+ release, newest first. Beta versions are opt-in previews; the
latest stable version is what `snapmap-plus update` installs.

## v0.2.1-beta.12 -- 2026-09-12 (beta)

**Map rendering controls and reliable Grid Room placement**

Adjust view distance and fog per map, place objects inside tall Grid Rooms, and restore the game's original environments when customization is off. Reset resized rooms and remove package-dependent content to return otherwise stock maps to vanilla compatibility.

### New
- DOOM's **Settings / Properties** panel now offers **Custom Rendering**, with view distance and fog strength, range and color controls saved inside each map on OpenGL and Vulkan.

### Improved
- Turning **Custom Rendering** off restores each module's original environment, while saved custom values remain available when re-enabled.
- Restoring every Grid Room to its original dimensions and removing package-dependent content permits otherwise stock maps to load in vanilla after saving with **Custom Rendering** off.

### Fixed
- Objects can be placed inside tall resized Grid Rooms, and resizing refreshes the faint native placement grid.
- **Grid Room Size** no longer inherits the **Grid Offset** field's 0-63 input range; dimensions still clamp to the door and portal limits.

## v0.2.1-beta.11 -- 2026-09-12 (beta)

**Resizable Grid Rooms, a rawmap File menu and steadier navigation**

Resize individual Grid Rooms in DOOM's Module Properties, open and export rawmaps through the File menu, and keep navigation previews steady after moving or duplicating boxes. Navigation baking runs in the background, with fixes for dense routes and rotated climb approaches.

### New
- Classic and modern Grid Rooms have independent X, Y and Z dimensions under **Grid Room Size** in DOOM's **Module Properties**, including Blueprint mode. Doors retain their size and placed objects retain their local positions and sizes; built-in lighting, collision and navigation adjust to the room. **Players need Snapmap+ to load maps with resized Grid Rooms; vanilla and console players cannot use them.**
- The **File** menu can load rawmaps and export the open map, including unsaved edits, with **Save Rawmap** and **Save Rawmap As...**. Opening a rawmap protects the saved map it opened over by default, and changing maps resets the export destination.

### Improved
- Navigation previews retain box shapes from the latest complete map snapshot instead of returning to stale spawn positions during live flag refreshes. Placement and module movement refresh the snapshot without requiring a save.
- Preview baking runs in the background to reduce editor stalls. Refused bakes report their reason; route-limit diagnostics can mark affected volumes red, and `sh_perf` reports editing costs.

### Fixed
- Duplicated boxes sharing an ID retain navigation when their owning module is unambiguous. Until the IDs are unique, refreshes read the whole map; IDs assigned across modules still refuse the bake.
- Dense bakes no longer hit the former 2,048-traversal candidate cutoff, and repeated path-search candidates cannot overflow the native queue. Engine route limits still apply.
- Climb approaches clear the physical rotated box and select traversal animations for each anchor height, repairing blocked approaches without changing the authored surface.

## v0.2.1-beta.10 -- 2026-09-10 (beta)

**Connected navigation for custom bridges and ramps**

Navigation now follows the exposed surfaces of rotated and intersecting Blocking Boxes, with repairs for demons crossing bridges and returning to module floors. Editing refreshes the bake automatically, and the green preview is visible on both renderers. Existing navigation marks migrate without manual retoggling.

### New
- Weapon packages can select ammo presentation from the weapon's capacity, including the existing infinity display, without changing ammunition or gameplay.

### Improved
- Rotated boxes, floating decks and intersecting structures bake from their exposed walkable surfaces. Shared supports retain contact, while buried surfaces are removed.
- Moving, rotating, copying or deleting boxes refreshes navigation. Each placed module keeps its own bake, including maps with multiple Grid Rooms.
- Platforms can connect directly, with gap leaps where the demon's native traversal data allows them. Modules that already contain climbs can receive additional custom traversal links.

### Fixed
- Repaired bridge and ramp-to-floor transitions by preserving floor contact, rebuilding navigation visibility and keeping marked volumes out of the native avoidance obstacle list. This also applies to maps saved with the replacement AI Navigation marker.
- Dense bakes respect the engine's connection limits. Candidates that cannot fit are refused before loading, preventing the associated Play failure.
- The green navigation preview now displays correctly in the OpenGL editor as well as Vulkan.

## v0.2.1-beta.9 -- 2026-09-08 (beta)

**Marks take effect on Play, and the bake no longer crashes**

Climb links are now placed to suit each demon, so more of your platforms are reachable, and marks ticked during a session reach the bake without a save-and-reload. Several crashes around map building and console commands are gone, and reports now say which renderer you were running.

### New
- Feedback and crash reports now carry which renderer version of the game you are running, shown as a Renderer line, so renderer-specific bugs can be told apart.

### Improved
- Climb links are placed along a ledge where each demon can actually stand, so a demon whose climb starts further out no longer loses the edge, and platforms that no demon could reach become reachable.
- Six dash and meathook tuning settings that nothing could ever read are no longer offered, and the documentation no longer lists them.

### Fixed
- Surfaces you mark during a session now reach the navigation bake without saving and reloading the map.
- The game no longer dies with a fatal error while building a map that has live navigation marks.
- Console commands that edit game data now run on the game's own thread, ending the crashes some players hit when using them.

_Plus 6 smaller fixes and internal changes._

## v0.2.1-beta.8 -- 2026-09-07 (beta)

**Demons walk and climb your own geometry**

Mark the Blocking Boxes you build in DOOM's own object settings and Snapmap+ bakes navigation onto them at map load, so demons walk and even climb onto your platforms. Snapmap+ also now runs on both the Vulkan and OpenGL versions of the game.

### New
- An AI Navigation switch on any Blocking Box, in DOOM's own object settings, marks its top surface as ground demons may walk on.
- Demons now climb onto marked surfaces well above step height, using the animation that suits each one.

### Improved
- Snapmap+ attaches to whichever DOOM executable is running, so everything keeps working after you switch between Vulkan and OpenGL.
- The installer now spots the game running under either renderer and asks you to close it instead of failing on a locked file.

### Fixed
- Marks made during a session survive pressing Play instead of being quietly discarded for the rest of the session.
- The Entity State tab accepts valid values again instead of rejecting them as unknown and suggesting ones the game refuses.

_Plus 23 smaller fixes and internal changes._

## v0.2.1-beta.7 -- 2026-09-03 (beta)

**Release notes you can actually read**

Each release now ships one short, hand-written entry instead of a wall of commit messages, shown the same way on the website, the releases page and in the mod's own history command. Snapmap+ also tidies away a stale log file left behind by an earlier rename.

### New
- The history command lays its notes out for a terminal and can show any version you name.

### Improved
- Every release now comes with one short, reviewed summary shared by the website, the releases page and the mod itself.

### Fixed
- The leftover log file from the old naming no longer sits alongside the current one.

## v0.2.1-beta.6 -- 2026-09-03 (beta)

**Mod packs install and play in one step**

A map that needs mod packs now installs all of them from a single prompt and
plays straight away, instead of asking once per pack and sending you back to
restart DOOM.

### New
- Install every mod pack a map needs from a single prompt.
- Play a map's mod pack straight after installing it, with no restart.

### Improved
- The mod-pack consent prompt uses a dialog that cannot delete anything.
- Logs and crash records are rolled aside instead of growing without limit.

### Fixed
- Choosing Yes on the mod-pack prompt now actually installs the pack.

_Plus 15 smaller fixes and internal changes._

## v0.2.1-beta.5 -- 2026-08-26 (beta)

**Packages carry their own images and text**

An override package can now ship the pictures and names for the content it
adds, so a mod that adds an entity no longer borrows another demon's Toybox
tile or asks you to hand-edit a file every other mod shares.

### New
- Packages can ship their own images and their own text.

### Improved
- Your own overrides move into a package you can name, share and uninstall.
- An added entity survives a playtest and stays usable in the editor.

### Fixed
- Two packages whose names differ only in letter case no longer conflict.

_Plus 3 smaller fixes and internal changes._

## v0.2.1-beta.4 -- 2026-08-21 (beta)

**Override packages and a real asset browser**

Overrides become self-contained packages that can overlap without refusing each
other, and the asset browser gains sound auditioning, pinning, folder trees and
29 new textures.

### New
- Add declarations DOOM never shipped, through override packages.
- Pin a shortlist of assets, and browse sounds by soundbank as a folder tree.

### Improved
- Overlapping override packages compose instead of refusing each other.
- Prefab previews are interactive, and the Studio workspace is unified.

### Fixed
- Every material is searchable, and a timeline larger than 1 MB opens.

_Plus 57 smaller fixes and internal changes._

## v0.2.1-beta.3 -- 2026-07-28 (beta)

**Settings that stick, and keyboard paging**

The editor remembers your Entities menu and override-loading preferences
between sessions, lists and dropdowns page from the keyboard, and text fields
copy and paste.

### New
- Page through lists and dropdowns from the keyboard, and copy and paste in
  text fields.

### Improved
- Entities menu and override-loading settings persist between sessions.

### Fixed
- Recovering from a fault during a play transition no longer causes a second
  crash.

_Plus 7 smaller fixes and internal changes._

## v0.2.1-beta.2 -- 2026-07-21 (beta)

**A community section on the site**

The website gains a Community section backed by GitHub Discussions: sign in with
GitHub to post, comment, reply and react, with search, sorting and tags.

### New
- Post, comment, reply and react in a new Community section, using your GitHub
  sign-in.
- Browse the community with search, sorting, tags and a rich-text composer.

### Improved
- The site is redesigned, with a mobile menu and a Discord link in the nav.

### Fixed
- Console commands for Snapmap+ settings work without turning on developer mode.
- Turning the user override layer off now sticks across restarts.

_Plus 18 smaller fixes and internal changes._

## v0.2.1-beta.1 -- 2026-07-19 (beta)

**SnapHak becomes Snapmap+**

The project is renamed Snapmap+, and your saved content moves into a folder of
its own under your local app data.

### New
- Snapmap+ keeps your maps, prefabs and overrides in their own folder.

### Improved
- Existing content is migrated once, and the move is verified before the old
  copy is removed.
- The installer carries a version resource and manifest, so fewer antivirus
  tools flag the download.

_Plus 1 smaller fix and internal change._
