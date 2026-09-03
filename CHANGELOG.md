# Changelog

Every Snapmap+ release, newest first. Beta versions are opt-in previews; the
latest stable version is what `snapmap-plus update` installs.

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
