# Private virtual-texture families: what a baker must write

## Purpose

DOOM 2016 draws most world and prop surfaces from virtual textures: pages of
diffuse, specular and normal detail streamed from page files. A mod can carry
its own family of pages, so its materials stream detail the same way as stock
ones. This document is the contract between a tool that writes such a family
(Doomforge's baker) and Snapmap+, which serves and registers it
(`src/backend/modvt.c`, `src/backend/modbundle.c`). Everything here is for
build 13954591 on both executables unless marked.

Status: the file layout, the material text, the registration and the drawing
of a family made from copied stock pages are confirmed in game on Vulkan. The
plane contents are confirmed by decoding stock pages, except the split of the
normal across the two alpha channels, which is inferred. Whether the page
codec is standard JPEG XR is not settled, so a new encoder must pass the
round trip in [Validation](#validation) before its output ships.

## Contents

1. [Names and files](#names-and-files)
2. [Geometry](#geometry)
3. [Member file](#member-file)
4. [Pages](#pages)
5. [Which pages to write](#which-pages-to-write)
6. [Material text](#material-text)
7. [Sizes and limits](#sizes-and-limits)
8. [What Snapmap+ does with it](#what-snapmap-does-with-it)
9. [Validation](#validation)
10. [Authored map families](#authored-map-families)
11. [Evidence](#evidence)

## Names and files

| Item | Value |
|---|---|
| Family name | `mods/<id>/<family>`, lower case. `<id>` is the mod id as `mod.json` spells it; folders below it are allowed (`mods/<id>/skins/imp`). It must not contain `_vmtr` (such a name binds the stock family). |
| Member files | `assets/virtualtextures/mods/<id>/<family>_sq<N>.mega2`, N from 1 to 16 without a leading zero. Write only the squares the family uses; any subset works. |
| Not written | No `_q<N>.mega2` members, no `.pages` file, no `.vmtr` table. The engine never reads a private family's `.vmtr` rows, so each material carries its own mapping. |
| Materials | Ordinary material decls under `generated/decls/material/mods/<id>/`, naming the family (see [Material text](#material-text)). |

The bundle builder admits a `.mega2` file only in exactly this form and only
under the mod's own id; any other `.mega2` path fails the build.

## Geometry

A mega2 family always has the same geometry, fixed by the engine:

- 12 mip levels; level 0 is 2048 x 2048 pages.
- A page holds 120 x 120 payload texels plus a 4-texel border on every side
  (128 x 128 decoded). Level 0 is therefore 245,760 texels per side.
- The family is cut into a 4 x 4 grid of squares. Square N (1 to 16) is at
  column `(N - 1) % 4`, row `(N - 1) / 4`; each covers 512 x 512 level-0 pages
  (61,440 texels per side). One member file holds one square.
- At level L a square covers `max(512 >> L, 1)` pages per side, at page
  `(col * 512) >> L, (row * 512) >> L` of that level's grid. Levels 9 to 11 are
  1 x 1 cells that several squares share (level 10 at `(col >> 1, row >> 1)`,
  level 11 at `(0, 0)`); every member stores its own copy of them.

## Member file

Little-endian. Layout: header (0x170 bytes), page payloads contiguous from
0x170, the cell index, then the page table at the end.

| Offset | Size | Value |
|---|---|---|
| 0x00 | u32 | magic `0xA63FBB21` |
| 0x04 | u32 | version 2 |
| 0x08 | u32 | 16 (squares in the family) |
| 0x0C | u32 | 12 (levels) |
| 0x10, 0x14 | u32 x 2 | 245760, 245760 |
| 0x18, 0x1C | u32 x 2 | 2048, 2048 |
| 0x20, 0x24 | u32 x 2 | 61440, 61440 |
| 0x28, 0x2C | u32 x 2 | 512, 512 |
| 0x30 | u32 | 2 |
| 0x34 | u32 | 12 |
| 0x38 | u64 | file offset of the page table |
| 0x40 | u64 | file offset of the cell index |
| 0x48 | u32 | page count |
| 0x4C | u32 | cell count, 349,527 (the sum of every level's cells) |
| 0x50 + 0x18 * L | 6 x u32 | level L: x, y, w, h (in pages, in that level's global grid), first cell, cell count |

Level table entries follow [Geometry](#geometry): `w = h = max(512 >> L, 1)`,
`count = w * h`, `first` is the running sum, so the twelve counts add up to
349,527.

- **Cell index**: 349,527 u32 page ids in level order, each level row-major
  over its `w x h` cells. `0xFFFFFFFF` is an empty cell. Several cells may name
  one page (identical pages may be shared).
- **Page table**: `page count` entries of `{u64 offset, u64 size}`; every entry
  lies inside the payload area, and payloads do not overlap.

The engine reads the header, the whole index and the whole page table when the
family is registered, then reads page payloads on demand.

## Pages

A stored page is a 16-byte header followed by its streams in this order:
plane 0, plane 1, plane 2, the extra section (unused here), plane 3, and the
0x800-byte cover bitmap when present.

| Offset | Field | Write |
|---|---|---|
| 0 | plane 0 quality | `0x1E` |
| 1 | plane 1 quality (also the second parameter of planes 1 and 2) | `0x1E` |
| 2 | plane 2 quality | `0x1E` |
| 3 | flags: `0x08` cover bitmap present, `0x04` plane 3 uses LZ4 (else LZW) | `0x04`, or `0x0C` with a cover bitmap |
| 4 | u16 **big-endian** plane 0 bytes | |
| 6 | u16 big-endian plane 1 bytes | |
| 8 | u16 big-endian plane 2 bytes | |
| 0x0A | u16 big-endian plane 3 compressed bytes | 0 when plane 3 is absent |
| 0x0C | u16 big-endian extra section bytes | 0 |
| 0x0E | absent flags: `0x01`/`0x02`/`0x04` plane 0/1/2 absent (zero-filled), `0x10` extra absent, `0x20` the cover is the constant byte at 0x0F, `0x40` plane 3 absent, `0x80` lightmap mode (never set) | `0x60` for a page with no colour mask and a constant cover (99 percent of stock pages) |
| 0x0F | constant cover byte | 0 |

The stored size must equal 16 + the five lengths + 0x800 when the cover bitmap
is present. Each stream is at most 65,535 bytes.

Planes 0 to 2 are HDP (HD Photo, JPEG XR class) streams of a 128 x 128 RGBA
image including the 4-texel border; plane 3 is a 128 x 128 BC7 colour mask
compressed with LZ4 and may be omitted. Decoded, the planes hold:

| Plane | Channel 0-2 | Channel 3 |
|---|---|---|
| 0 | albedo R, G, B (plain RGB, not YCoCg) | tangent-space normal X |
| 1 | specular R, G, B | tangent-space normal Y |
| 2 | 0, 255, then the cover channel (255 when no cover bitmap is stored) | power (smoothness) |

The normal's Z is rebuilt by the shader. Emissive has no plane: keep it as an
ordinary image on the material. The cover bitmap, when used, is 0x4000 bits,
least significant bit first, row-major over the 128 x 128 page; a set bit
writes 255 into plane 2 channel 2.

## Which pages to write

Each material occupies one rectangle of the family:

- Resample an N x N source to N x 15/16 texels per side (2048 becomes 1920,
  that is 16 pages; 128 becomes 120, one page), the engine's own ladder.
  Non-square sources keep their aspect in whole pages.
- Place the rectangle at x, y, w, h that are multiples of 120 texels, inside
  one square (a rectangle never crosses a square edge), aligned to its own size
  where possible.
- For every level L from 0 to 11, downsample the square's composite by 2^L,
  cut it into 120-texel pages on that level's grid, and write a page for every
  cell the rectangle touches at that level, including the shared level 9 to 11
  cells. A page's 4-texel border comes from the neighbouring composite texels,
  clamped at the square edge. All other cells stay `0xFFFFFFFF`.

Example (the reference family): one 960 x 960 texel rectangle (8 x 8 pages)
needs 64, 16, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1 pages at levels 0 to 11, 93 pages
in all.

## Material text

Keep the material's ordinary `diffusemap`, `bumpmap`, `specularmap` and
`powermap` images (the engine requires one of them, and they are the fallback),
and add:

```
landpagefile "mods/<id>/<family>"
usevirtualmapping 1
virtualmapping { w/245760, h/245760, x/245760, y/245760 }
virtualtexturefeedbackfloat { 1, 2048, 245760, 16 }
```

The `virtualmapping` order is w, h, x, y, each the rectangle's texels divided
by 245,760, written with at least 8 significant digits. Do not write
`pagetablemap _vmtrpagetable` or the physical page image parameters. The
`landpagefile` string must equal the family name exactly.

## Sizes and limits

| Limit | Value |
|---|---|
| Member file | at most 64 MiB (the bundle builder's per-file cap). The cell index alone is 1,398,108 bytes and the header 368, so about 62.6 MB is left for pages. |
| Bundle | at most 256 MiB and 4096 files in all. |
| Families per bundle | no limit of its own; each family takes one record. |
| Records | 30 for everything the game and mods register at once (see below). |
| Memory per family | about 14 MB while registered (13.9 MB measured on Vulkan): 11,184,810 bytes of host page table, the member's index and page table, and a 2048 x 2048, 12-level page-table image. |

Measured sizes of the reference family's one member, `fam_sq1.mega2`:
2,955,715 bytes = 368 header + 1,555,751 bytes of 93 pages (7,023 to 42,288
bytes, about 16.7 KB each) + 1,398,108 index + 1,488 page table. At that page
size a 1920 x 1920 material (348 pages over all levels: 256, 64, 16, 4, 1 and
seven shared coarse cells) costs about 5.8 MB, so one 64 MiB member holds
about ten such materials, and the 256 MiB bundle cap, not the member cap,
limits a mod to roughly forty.

## What Snapmap+ does with it

- The members are served through the game's resource provider like any other
  bundle file.
- One page-source record per family is created with
  `SetSource(record, "mods/<id>/<family>", "virtualtextures/mods/<id>/<family>.pages", flavor, 2)`
  when the mod is installed, or for mods present at startup at the first
  editor entry or map load, always before any of the mod's materials is read.
  A material read before its family exists is bound as an ordinary unique
  family instead, without the mod's record, for the rest of the session.
- The game has 31 records; the product uses 1 to 30 and never evicts a record
  it did not create. A family that finds them full is refused with one
  `VT:` log line.
- On OpenGL a family is registered only when the AMD sparse texture extension
  and its cvar are on (the game then uses sparse page tables); otherwise it is
  not registered and the log says so.
- Removing the mod releases its family's record, unless the editor or a map
  was opened while it was registered; then the record stays until DOOM exits.
- `vt_reloadMega2Vmtrs` does not reload a served family.

## Validation

The product checks only the member's path. A baker must check, before it
writes the bundle:

1. Every member: magic, version 2, the constants above, a level table
   consistent with its square, the index and page table inside the file,
   every non-empty id below the page count, every page inside the payload area
   with no overlap.
2. Every page: stored size = 16 + streams (+ 0x800 with a cover bitmap), with
   big-endian lengths and consistent absent flags; each stream at most 65,535
   bytes; flag 0x80 never set.
3. Round trip: each page decodes through the game's own page decoder (swap the
   five big-endian lengths first, as the game does) and each plane's 120 x 120
   core matches the source within the tool's PSNR threshold. `sh_vt decode`
   runs exactly that decoder in game and writes the five planes.
4. Coverage: every material rectangle's cells non-empty at all 12 levels, no
   rectangle outside the family or crossing a square, no two overlapping, every
   material's `landpagefile` naming this family and its `virtualmapping` equal
   to its rectangle / 245,760 within 1e-6.
5. Names: family under `mods/<id>/`, no `_vmtr`, `_sq` members only.

## Authored map families

A map's own unique virtual texture (the kind every stock map and SnapMap module
uses, a single `.pages` file of format version 3 that the engine opens itself)
is a different format and is not written by this contract. A stock multiplayer
map's `.pages` file needs nothing from a baker. An authored map would need
either a `.pages` version 3 writer, or a mega2 family as above, fixed at 2048
pages and 12 levels. A `.pages` file is also refused by the bundle builder today
(the file type is not admitted, and real map files exceed the 64 MiB member cap:
stock multiplayer maps' are 110 to 516 MB), so carrying one needs a builder
change and a larger cap for that file type.

## Evidence

The format and the engine behaviour were established in snaphak-re's
mod-system-reinstatement campaign: lane RB (static analysis of build 13954591:
member loader, provider header, page header, material parse, record lookup),
lane RC (the reference family, `make_family.py`, a sparse copy of stock
`_vmtr_sq1.mega2` pages for the stock rocket launcher prop, and the in-game
decoder probe that identified the planes), and lane P5 (this registration).
The reference family drew identically to the stock prop on Vulkan; OpenGL is
not yet tested in game.
