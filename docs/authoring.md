# Making a mod

A mod is a folder in `%LOCALAPPDATA%\snapmap-plus\mods\`: a `mod.json` and folders named for what the files in them
are. You name your files by their place in that folder and refer to them by the same names. Snapmap+ gives the mod
its identity and keeps its names apart from the game's and from every other mod's; you never write it.

## The folder

| Folder | Holds | The game's name for it |
|---|---|---|
| `materials/` | materials (`.decl`), including the [Graphics SDK's](graphics-sdk.md) `type layered` and `type particle` | material |
| `images/` | images (`.bimage`) | image |
| `meshes/` | models (`<name>.md6.decl`), their meshes (`.bmd6model`) and skeletons (`.bmd6skl`), static models (`.bmodel`), destructible models (`.dmodel`), particle models (`.pmodel`) | md6Def, model |
| `animations/` | animation clips (`.bmd6anim`) | anim |
| `effects/` | effects (`.decl`): lights, decals, particles, shakes, ribbons, played together | fx |
| `particles/` | particle systems (`.decl`) | particle |
| `entities/` | entity definitions (`.decl`): a demon, a prop, a pickup | entityDef |
| `sounds/` | sound definitions (`.decl`) and Wwise banks (`.bnk`, in `sounds/` itself) | sound |
| `scripts/` | Lua scripts that run (`.lua`), and in `scripts/lib/` the ones loaded with `require` | |
| `movies/` | Flash movies (`.bswf`) and their atlases | swf |
| `programs/` | render programs, for the rare mod that carries its own (`permutations/` and `spirv/` below it) | renderProg |
| `map-modules/` | SnapMap modules and the files the game opens beside them | snapModule |
| `virtual-textures/` | a private virtual-texture family (`vt-baker.md`) | |
| `atlases/decals/`, `atlases/lights/`, `atlases/transparency/` | atlas images a converter writes | |
| `decls/<kind>/` | every other kind of decl, in a folder named as the game names the kind: `decls/damage/`, `decls/snapeditorentitydef/` (Toybox rows), `decls/breakable/` | |
| `game/` | the game's own files your mod changes or carries, at the game's own path: `game/generated/decls/entitydef/ai/conductor.decl` | |

A file beside `mod.json` (a README, notes) and a folder whose name starts with `.` are not part of the mod. Any other
folder is a mistake the build names.

## Names

**A name that begins with one of your folders is yours; every other name is the game's.**

- In a decl you write `materials/crate_glow`, `images/crate/rust.tga`, `meshes/boss/body.md6`,
  `animations/boss/walk.md6anim`, `effects/sparks`, `entities/crate`, `decls/damage/crate_burst`. A decl is named by its
  path without `.decl`; an image as materials name images, with `.tga` for the `.bimage` file.
- `models/monsters/imp/imp_skin`, `textures/guis/...`, `art/...` and every other name are the game's own, written as
  the game writes them.
- The game keeps a few names that begin like one of your folders (452 under `effects/`, one under `particles/`). To
  name one of those, write `game:` before it: `game:effects/explosion`.
- Strings: a key in `mod.json`'s `strings` is yours, and a decl names it `#str_<key>`. A key that is one of the game's
  own changes the game's text where your mod is used.
- In a script a call that takes one of your mod's things takes its name without the folder (`play_effect("sparks",
  pos)` plays `effects/sparks.decl`, `spawn("crate", pos)` places `entities/crate.decl`); `game("...")` names the
  game's.

A name of your mod that names nothing your mod has stops the build with the file that names it (an image a
virtual-textured material names, and a name the game's own text also writes, are reported without stopping it).

## A small mod from an empty folder

A crate that throws sparks and glows where it is hit. It draws the game's own SnapMap crate and the game's own sparks,
so the mod carries no binary file. (This example is built and served offline by the product's tests; see "What the
build checks".)

1. In the Mods tab press **New mod**, or make the folder yourself:

   ```
   %LOCALAPPDATA%\snapmap-plus\mods\spark-crate\
     mod.json
     entities\crate.decl
     effects\crate_sparks.decl
     decls\snapeditorentitydef\crate.decl
     scripts\crate.lua
   ```

2. `mod.json`, the mod's own part (Snapmap+ adds its own part, `"snapmap-plus"`, the first time it scans the folder):

   ```json
   {
     "name": "Spark crate",
     "description": "A crate that throws sparks where it is hit.",
     "author": "you",
     "strings": {
       "en": {
         "crate_name": "Spark crate",
         "crate_desc": "Throws sparks and glows where it is hit.",
         "crate_category": "Spark crate"
       }
     }
   }
   ```

3. `entities/crate.decl`, the game's SnapMap crate under a name of the mod's own:

   ```
   {
   	class = "idProp_Coop";
   	edit = {
   		renderModelInfo = {
   			model = "models/snapmaps/props/new/crate01.lwo";
   		}
   		clipModelInfo = {
   			type = "CLIPMODEL_AUTO";
   		}
   		dynamicObstacle = true;
   	}
   }
   ```

4. `effects/crate_sparks.decl`, one of the game's own spark particles, played once:

   ```
   {
   	edit = {
   		editEvents = {
   			num = 1;
   			item[0] = {
   				name = "sparks";
   				type = "FX_PARTICLE";
   				duration = 1;
   				startCondition = {
   					num = 1;
   					item[0] = "FX_NONE";
   				}
   				stopCondition = {
   					num = 1;
   					item[0] = "FX_NONE";
   				}
   				particleParms = {
   					declPrt = "env/sparks/sparks_med_01";
   				}
   			}
   		}
   	}
   }
   ```

5. `decls/snapeditorentitydef/crate.decl`, the crate's Toybox row (its own name and category, the game's icon):

   ```
   {
   	inherit = "prop/prop_base";
   	edit = {
   		previewImage = "textures/guis/snapmaps/entity_icons/prop/crate01";
   		categoryNameTag = "#str_crate_category";
   		displayNameTag = "#str_crate_name";
   		displayDescriptionTag = "#str_crate_desc";
   		entityDef = "entities/crate";
   	}
   }
   ```

6. `scripts/crate.lua`:

   ```lua
   api = 1

   on.damage("crate", function(crate, hit)
     play_effect("crate_sparks", hit.location)
     crate:glow(1, 0.45, 0.1, 0.3)   -- an orange glow, fading in over 0.3 seconds
   end)
   ```

7. Press **Refresh** in the Mods tab. The build reads every file as the game will and says in one sentence what to
   change if something is wrong. Open SnapMap; the crate is in the Toybox under **Spark crate**.

What the game is handed is the same files under the mod's identity: `entities/crate.decl` is served as
`generated/decls/entitydef/mods/<salt>/crate.decl`, the row's `entityDef` as `mods/<salt>/crate`, `#str_crate_name`
as `#str_<salt>_crate_name`. A material of your own goes in `materials/` and is named the same way
(`customMaterial = "materials/crate_rust"`); the [Graphics SDK](graphics-sdk.md) shows two.
## What the build checks

Every file is checked when the mod is built (a Refresh, or the first scan of a folder): its folder and kind, every
decl as the game parses it, every name a decl writes against what the mod and the game hold, every script with the
Lua compiler, every Graphics SDK material against its fields. A mistake stops the build with one sentence naming the
file, the line and what to write instead; the mod then serves nothing until it is fixed. Nothing about the game's
limits is reported: the product fits every mod into them.

## For converters

A converter (Doomforge) writes this same folder: it composes the game's own layout and translates it as its last
step (snaphak-re `doomforge/doom2016/modfolder/authoring.py`), checking that the product's reading of every text gives
back the bytes the converter meant the game to read.
