# The Graphics SDK

A mod describes how its things look in plain terms; Snapmap+ turns the description into DOOM 2016's own material
and effect data. Snapmap+ uses the game's own render programs and adapts them when an effect needs additional
texture or shading inputs. An author
never names a program, a render parm, a texture slot or a renderer, and a material never fails to load because of a
limit: the product packs and budgets it, and reports only a mistake in the description, in one sentence that says
what to write instead.

The SDK has these parts:

- **Material types in data** use `type` as the first key. Use `layered` for tiled surfaces, `particle` for particle
  shading, `ribbon` for trails and `recolourable` for a model whose colours can change in Play.
- **Effects in data**: an effect (`effects/x.decl`) whose first key is `type effect` names what plays in plain
  items: a light, a decal, a shake, a flash, a screen image, a blur, a focus change, a flare, a trail, a model, a
  sound, a glow, an environment change, wind and particles ([Effects](#effects), [Particles](#particles)). The
  product writes the game's own fx decl from it. An fx decl written as the game writes them still works as before.
- **Values that change over time**: `pulse`, `flicker`, `fade` and `curve` wherever a number or a colour goes
  ([Values that change](#values-that-change-over-time)).
- **Environments in data**: an environment (`environments/x.decl`) whose first key is `type environment`
  ([Environments](#environments)).
- **Wounds in data**: a wound site (`decls/gorewounds/x.decl`) whose first key is `type wound` ([Wounds](#wounds)).
- **Script calls**: `play_effect`, `move_effect`, `stop_effect`, `end_effects_at_once`, `set_environment`,
  `environment_area`, `clear_environment` and the entity calls `tint`, `glow` and `recolour` ([Scripts](#scripts), and
  [Mod scripts](architecture.md#mod-scripts-entity-animation-and-world-calls)).

Every word an author writes is listed once in the product (`src/backend/sdkwords.h`), so the compilers, their
mistakes and this guide name the same words.

## A layered surface

A layered surface is a base surface (the material's own virtual-texture pages, as any skinned material has) with up
to four tiled layers laid over it. A mask image picks where each layer shows (its red channel layer 1, green
layer 2, blue layer 3, alpha layer 4); where layers meet, their height images decide the edge, so rock shows
through flesh in its cracks first.

```
{
	type			layered
	wounds			yes
	// the base surface: the material's own keys, as for any skinned material (virtual texture, bloom, ...)
	layermask		images/demon/body_mask.tga
	layer1			images/tiles/pores.tga
	layer1normal		images/tiles/pores_n.tga
	layer1height		images/tiles/pores_h.tga
	layer1repeat		20
	layer2			images/tiles/rock.tga
	layer2normal		images/tiles/rock_n.tga
	layer2repeat		3
	layer2weights		{ 0.2, 0.5, 0, 0, 1 }
	layer3			{ 0.35, 0.05, 0.03 }
	burn			images/tiles/burnt.tga
	burnheat		{ 1, 0.07, 0 }
}
```

Every field is optional; a field left out has the value in the table.

| Field | Value | Default | What it does |
|---|---|---|---|
| `wounds` | `yes` / `no` | `no` | the surface takes 2016's wound and gore look (a demon's body and wound materials) |
| `layermask` | image | none (no layer shows) | channel *i* selects layer *i* |
| `layer<i>` | image or `{ r, g, b }` | none (the base colour stays under the layer) | the layer's colour |
| `layer<i>normal` | image | flat | the layer's normal map |
| `layer<i>smoothness` | image or value | 0.5 | the layer's smoothness |
| `layer<i>height` | image or value | 0.5 | the layer's height, for the edge between layers |
| `layer<i>specular` | image or `{ r, g, b }` | `{ 0, 0, 0 }` | |
| `layer<i>glow` | image or `{ r, g, b }` | `{ 0, 0, 0 }` | light the layer gives off |
| `layer<i>repeat` | value | 1 | how many times the layer repeats across the surface |
| `layer<i>tiling` | `{ u, v }` | `{ 1, 1 }` | a further repeat per direction |
| `layer<i>scroll` | `{ u, v }` | `{ 0, 0 }` | movement, in repeats per second |
| `layer<i>coverage` | value | 1 | how strongly the mask selects the layer |
| `layer<i>heightblend` | value | 1 | how much the heights decide the edge |
| `layer<i>softness` | value | 0.25 | how soft the edge is |
| `layer<i>weights` | `{ colour, normal, specular, smoothness, glow }` | `{ 1, 1, 1, 1, 1 }` | how far each property moves toward the layer (0 keeps the base, 1 takes the layer; below 0 or above 1 exaggerates) |
| `baseheight` | image or value | 0.5 | the base surface's height |
| `baseheightblend` | value | 1 | |
| `basetiling`, `basescroll` | `{ u, v }` | `{ 1, 1 }`, `{ 0, 0 }` | for the base height image |
| `baseglow` | `{ r, g, b }` | `{ 1, 1, 1 }` | scales the base surface's own glow |
| `burn` | image | none (no burn) | a burnt tile the surface turns to where its wounds bleed, with glowing embers |
| `burnglow` | image | none | the base's glow while burning (read only with `burn`) |
| `burnblood` | image | none | the wound cap's blood, which drives the burn (read only with `burn`) |
| `burntiling` | `{ u, v }` | `{ 4, 4 }` | |
| `burnnoise` | `{ tiling1, tiling2, speed1, speed2 }` | `{ 1, 4, 0.7, 0.4 }` | the embers' flicker: two scrolling noises, each with its own tiling and speed |
| `burnheat` | `{ r, g, b }` | `{ 1, 0.07, 0 }` | the embers' colour |

A layer that sets only numbers shows nothing: a layered material is drawn with the game's own skinned program unless it has a
`layermask` and at least one layer image or colour (any of `layer<i>`, `layer<i>normal`, `layer<i>smoothness`,
`layer<i>height`, `layer<i>specular`, `layer<i>glow`), a `burn` or a `baseglow`. A layer that has fields but no colour
of its own leaves the base colour alone (unless you write its `weights`).

A value or a colour where an image is expected (`layer3 { 0.35, 0.05, 0.03 }`, `layer1smoothness 0.8`) is a plain
colour; the product makes the image from it. Images for specular, glow, the base height and the burn (`burn`,
`burnglow`, `burnblood`, `baseheight`, then each layer's specular and glow in layer order) share two places; a surface
that asks for more than fits draws the ones that come last as their default: no blood, a flat base height of 0.5,
black specular or glow. The build log names the fields that use a default.

## A particle material

A particle stage names a material; a material with `type particle` decides how that stage is shaded. The stage
itself (its image, colours, fades, flipbook and motion) stays in the particle decl.

```
{
	type			particle
	transmap		images/fx/embers.tga
	lighting		lit
	glow			4
	glowmask		images/fx/embers_mask.tga
	glowmaskcolour		{ 1, 0.4, 0.1 }
	nearfade		{ 40, 8 }
	softdistance		5
}
```

| Field | Value | Default | What it does |
|---|---|---|---|
| `lighting` | `unlit`, `lit`, `explosion` | `unlit` | `lit` takes the level's light; `explosion` lights the smoke and makes the fire glow along the glow mask |
| `glow` | value | 1 | how bright the particle is |
| `glowmask` | image (a sheet like the particle's image) | none | where the particle glows on its own |
| `glowmaskcolour` | `{ r, g, b }` | `{ 1, 1, 1 }` | the glow's colour (`lit`) |
| `glowmaskcolour2` | `{ r, g, b }` | none | for lit particles, the second colour of the glow mask. Its red channel blends from the first colour to this one and controls glow strength |
| `glowmaskstrength` | value | 0 | the glow's strength (`lit`) |
| `gradient` | `{ r, g, b, r, g, b }` | white to white | the fire's colours along the glow mask (`explosion`) |
| `nearfade` | `{ begin, end }` | none | fades the particle out between two distances from the viewer (units) |
| `farfade` | `{ begin, end }` | none | fades the particle out from the first camera depth to the second, in game units |
| `softdistance` | value | none | fades the particle where it meets a surface, over this many units |
| `erosion` | `yes` or `no` | `no` | uses the texture's green channel as an erosion mask. Set the stage's generic parameter 1 from zero (gone) to one (full mask). |
| `depthoffset` | value | 0 | pushes the soft edge into thin parts of the image |
| `litscale` | value | 1 | scales the light a `lit` particle takes |

Simple lit and unlit particles use the game's stock shading. Their `glow` sets the material's own brightness.
Explosion shading, erosion, a glow mask, a gradient, either camera fade, soft distance and depth offset use the
Snapmap+ particle program. Lit particles also use it when `litscale` differs from 1 or `glowmaskstrength` differs from 0.

## Effects

An effect is a file in the mod's `effects/` folder (`effects/<name>.decl`) whose first key is `type effect`.
It lists the items that play together. Each item says when it starts, how long it lasts and how it fades.

```
{
	type		effect
	lasts		2			// every item but sound and particles that names no lasts of its own lasts 2 seconds
	light {
		colour		{ 1, 0.45, 0.1 }
		brightness	flicker { rate 8, low 3, high 6 }
		reach		300
		flicker		fire
		fadeout		0.5
	}
	decal {
		material	textures/decals/blood_splatter/blood_splatter_01
		size		64
		lasts		10
		fadeout		2
	}
	shake {
		strength	0.4
		lasts		0.5
		reach		800			// players farther away feel nothing
	}
	flash {
		brightness	0.6
		lasts		0.2
		reach		600
	}
	particles {
		image		images/fx/ember.tga
		count		40
		life		{ 0.5, 1 }
		from		sphere 16
		speed		{ 80, 160 }
		spread		30
		fall		-50
		size		curve { 0 3, 0.3 8, 1 0 }
	}
	sound {
		sound		native:effects/explosions/grenade_explosion_default
	}
}
```

A name in a field is the mod's when it begins with one of the mod's folders (`images/fx/ember.tga`,
`materials/rune`, `environments/hell_red`); any other name is the game's. A game name that begins like one of the
mod's folders is written `native:` before it (`native:effects/...`); `native("...")` also names DOOM's. A field left out keeps the game's own value for it, except where that draws nothing (a decal is given
10 seconds and 64 units, a light 300 units of reach).

**Every item** takes these fields:

| Field | Value | What it does |
|---|---|---|
| `starts` | seconds, or `{ least, most }` | waits before the item starts (the game draws between the two) |
| `lasts` | seconds | how long it plays (the effect's own `lasts` for an item that names none, except `sound` and `particles`) |
| `fadein`, `fadeout` | seconds | how long it takes to fade in and out (for particles: fractions of each particle's life) |
| `repeats` | `yes` / `no` | starts again when it ends, while the effect plays |
| `restarts` | `yes` / `no` | starts again when it is asked to start while it plays |
| `reach`, `fadesfrom` | units | (not a light) how far a shake, flash or flare reaches, and where it starts to weaken |
| `offset` | `{ x, y, z }` | where it plays, from the effect's place |
| `turn` | `{ pitch, yaw, roll }` | turns it from the effect's facing |
| `follows` | `yes` / `no` | follows the effect when it moves |
| `facing` | `follows`, `fixed`, `parent` | how it faces: with the effect, fixed, or with the entity the effect is on |
| `at` | a tag, or `{ tag, tag, ... }` | on an entity's own effect: the tags it plays at |
| `everytag` | `yes` / `no` | plays at every tag of `at` (`no`: the one the game picks) |
| `when`, `until` | a condition, or `{ ... }` | on an entity's own effect: the conditions that start and stop it (`spawn_start`, `death_1`, ...) |

**The items** and their own fields:

| Item | Fields | What it draws |
|---|---|---|
| `light` | `colour`, `brightness` (a number or a [changing value](#values-that-change-over-time)), `reach` (units, or `{ x, y, z }`), `flicker` (`none`, `flicker`, `fire`), `shape` (a light material), `shine` (specular), `shadows` (`yes`/`no`), `seenfrom` (units) | a light |
| `decal` | `material`, `size`, `depth`, `angle`, `glow` (`{ r, g, b, strength }`), `glowlasts` (seconds), `colour`, `stays` (`yes`/`no`) | a decal projected where the effect faces |
| `shake` | `strength`, `angles` (`{ pitch, yaw, roll }`), `move` (`{ x, y, z }`), `use` (one of the game's advanced shakes) | shakes the view of every player in reach |
| `flash` | `brightness` | flashes the view of every player in reach |
| `screen` | `material`, `colour` (three numbers), `tiles` (`{ x, y }`), `mirror`, `bottom` | an image over the view |
| `blur` | `strength`, `style` (`radial`, `soft`) | blurs the view |
| `focus` | `near`, `focus`, `far` | depth of field |
| `flare` | `use` (a flare), `size`, `colour` | a lens flare |
| `trail` | `use` (a ribbon) | a trail |
| `model` | `model`, `material`, `colour`, `size` | a model |
| `sound` | `sound`, `stops` (`yes`: stops with the item) | a sound |
| `glow` | `colour` (three or four numbers), `strength`, `value` (the render parm, default `bloomcolor`), `stays` | fades one of the entity's own values |
| `environment` | `use` (one of the mod's environments, `environments/<name>`, or a game env decl), or any field of an [environment](#environments) | changes how the level looks while it plays |
| `wind` | `strength`, `angle` | wind |
| `particles` | [Particles](#particles), or `use` (a particle decl), `size`, `colour`, `onscreen` | particles |

A colour is `{ r, g, b }`, or `{ r, g, b, opacity }` (a `screen` item's colour takes the three numbers only).

An effect is played from a script ([Scripts](#scripts)), or named where the game names an fx (an entityDef's
`fxDecl`, written `effects/<name>`): the product writes its fx decl under the same name, so nothing else changes.

## Particles

A `particles` item describes particles of its own. The product writes the game's particle decl for it, and its
material when it names an `image`.

The SDK calculates the visible bounds for the whole effect and for each stage. If you supply
a native GPU particle in `particles/`, include `edit.bounds` around the full emission volume.

Doomforge preserves an imported emitter's directional spray, including the variation in its horizontal
and vertical opening angles. Turning the emitter turns its spray.

```
particles {
	image		images/fx/ember_sheet.tga
	frames		{ 4, 4 }		// a sheet of frames, played at framerate a second
	framerate	12
	count		300
	life		{ 0.6, 1.4 }		// seconds, each particle between the two
	from		sphere 24		// point, sphere r, ring r, disc r, box { x, y, z }; 'surface' before it: the surface only
	direction	up			// up or outward; turn the item to aim it elsewhere
	spread		25			// degrees
	speed		{ 80, 160 }
	fall		400			// units a second squared; below 0 rises
	drag		0.5			// slows each particle toward still air (a second)
	size		curve { 0 4, 0.3 10, 1 0 }	// over each particle's life
	colour		fade { from { 1, 0.8, 0.3 }, to { 0.6, 0.05, 0 } }
	opacity		1
	brightness	1			// at most 1: the game draws a stage no brighter (give the image's glow instead)
	fadein		0.1			// fractions of the life
	fadeout		0.4
	angle		{ 0, 360 }		// each particle's starting angle
	faces		camera			// camera, motion (stretched along its flight), ground, free
	lighting	unlit			// unlit or lit (with an image)
	glow		2			// how bright the image is (with an image)
	bounce		0.3			// bounces off the surfaces under and behind the effect; stick, or none
	burst		yes			// every particle at once (no: spread over a life)
	repeats		yes			// emits for as long as the effect plays
}
```

`size` takes a number, two numbers (each particle between them), or a
[changing value](#values-that-change-over-time) over the particle's life; `colour` takes a colour or a changing
value; `opacity` takes one number. A speed range, drag and bounce are
Snapmap+'s simulated particle motion (see [architecture.md](architecture.md#simulated-particle-motion)); everything else is
the game's own particle stage. Write the total number of particles in `count`. Snapmap+ divides large counts
across CPU stages so the game can draw them. Other visible effects share the game's frame budget.

## Values that change over time

Wherever a number or a colour goes, write one of these instead:

| Form | Example | Means |
|---|---|---|
| `pulse` | `bloomcolor pulse { rate 2, low { 0, 0, 0 }, high { 1, 0.5, 0 } }` | a smooth rise and fall, `rate` times a second |
| `flicker` | `brightness flicker { rate 10, low 0.6, high 1 }` | a random smooth wobble |
| `fade` | `colour fade { from { 1, 1, 1 }, to { 1, 0, 0 }, over 3 }` | once, from one value to another (on an effect item or a particle) |
| `curve` | `size curve { 0 2, 0.2 8, 1 0 }` | points `time value`; on a material the time is seconds and `loop yes` is needed; on a particle 0 to 1 of its life |

On a material the value is the game's own register program over time and a table, so it costs no script time: any
of the material's vector keys may change (`bloomcolor`, `color`, the base material's keys of a look). A material has
no start, so it takes `pulse`, `flicker` and a looping `curve`; fade an entity from a script instead. A colour key the
game decodes from sRGB is decoded the same way, so a changing colour matches the same colour written as a constant.

## Environments

An environment is a file in the mod's `environments/` folder (`environments/<name>.decl`) whose first key is
`type environment`. It names what changes in the level's look; everything else stays the level's own.

```
{
	type		environment
	fog		{ 0.35, 0.05, 0.03 }
	fogdensity	0.03
	fogstart	200
	fogend		4000
	bloom		1.5
	exposure	{ 2, 10 }		// the darkest and the brightest the eye adapts to
	saturation	0.6
	tint		{ 1, 0.9, 0.8 }
	brightness	1.1
	vignette	pulse { rate 1, low 0.4, high 0.8 }
	vignettecolour	{ 0.2, 0, 0 }
	grain		0.03
	fringe		0.1			// colour fringes at the screen's edge
	blend		1.5			// seconds to blend in and out
}
```

`set_environment("hell_red")` (the file `environments/hell_red.decl`) lays it over the players' views, blending in over `blend`; `clear_environment()`
blends back to the level's own. An effect's `environment` item does the same for as long as the item plays. Each is
undone when the map ends. A value that changes over time is held at its middle by `set_environment` (the build log
says so); the environment itself, used for an area, draws it changing.

Use `environment_area(name, corner, opposite)` for a box players can walk into and out of. For example,
`local area = environment_area("hell_red", {x=0,y=0,z=0}, {x=600,y=800,z=240})` gives a room its own
environment. The game blends according to the current view and gives smaller overlapping boxes priority.
The area lasts until the script ends, or until you call `area:remove()`. The call returns nil when the box
is already occupied or the game's area list is full.

## A sky opening

A model surface using a sky material shows a cubemap through its silhouette. The sky stays aligned with the viewing direction as the player moves past the opening. Props and walls in front of the opening hide it in the usual way.

```
{
    type  sky
    image env/snapmaps/hell_px$bc7
}
```

Use a stock cubemap as above, or a cubemap of your own in `images/`. Apply the material to a surface on your model, such as a window or the inside of a dome.

## A recolourable surface

Add `type recolourable` to a skinned material to paint parts of a model independently. Keep the base surface and
its virtual-texture mapping. Supply a region mask as an ordinary linear image in the model's UVs, or omit
`regions` on a material whose baked virtual texture already carries a DOOM 2016 customization mask.

```
{
    type       recolourable
    regions    images/armour_mask.tga
    primary    { 0.2, 0.45, 0.8 }
    secondary  { 0.9, 0.6, 0.15 }
    shine      0.4
}
```

Paint the red and blue channels around a neutral value of 128. Above 130, red selects the primary colour
and blue selects the secondary colour. Below 125, red selects glow and blue selects pattern. The strength
reaches full at the channel endpoints. The green channel controls wear, with high values for scratches and
low values for dirt.

Use `pattern` for a tiling image and `patterncolours { r, g, b, r, g, b }` for its two colours. Optional
`scratches`, `dirt` and `glow` each take an RGB colour. Use `detail` for a shared tiling normal image and
`shine` for coating smoothness from 0 to 1. Colours range from 0 to 1. In script, call
`e:recolour{ primary = {1, 0, 0}, secondary = {0, 0, 1} }` on an owned entity to change its copy.

## Ribbon materials

A script can draw a path through moving joints or positions with `emitter:draw_ribbon(points, rgba)`.
Define an entity that inherits `func/ribbonemitter`. Set its `edit.declRibbon` to your ribbon declaration
and `edit.startOff` to `false` so the engine initializes its renderer at spawn. For example, spawn
`entities/effects/trail.decl` with `spawn("effects/trail", points[1])`. Pass 2 to 128 world positions in path order. The optional colour has four
channels from 0 to 1. Update the path while the effect plays and remove the emitter when it ends.

A material with `type ribbon` draws with the game's ribbon program. Set `lengthfade { 0.1, 0.2 }` to fade in
across the first tenth of the trail and fade out across its last fifth. The ribbon declaration supplies the
length coordinate with `textureTangentStretch = true;`. Its main texture can tile and scroll independently.

For a colour change over a node's life, give the material a `colour` and a `deathcolour`. Each takes four
linear RGBA channels from 0 to 1. `lifefade { 0.1, 0.3 }` fades the first tenth and last three tenths of the
node's life. Set the ribbon declaration's `fadeInFraction = 0;` and `fadeOutFraction = 1;` so the material
receives the node's age. A white ribbon colour uses the material's colours in full.

Doomforge also preserves animated emitter width on imported projectile trails. Width follows the time since
launch, so a growing trail widens along its whole visible length. Each node still expires at its authored lifetime.

## Screen text

Use `show_text(player, place, text, [colour], [seconds])` to put a message on a player's HUD. The places are
`"top left"`, `"top right"` and `"bottom"`. Each mod can update its own text in each place. An empty string clears
it. A positive duration clears the message after that many seconds. With no duration, it stays until the mod
replaces it or the map ends.

```lua
show_text(player, "bottom", "Door unlocked", {1, 0.8, 0.2}, 3)
```

## Wounds

A wound is a file in the mod's `decls/gorewounds/` folder (`decls/gorewounds/<name>.decl`) whose first key is
`type wound`: one place a demon can be wounded. It is written over DOOM 2016's own default wound and bleeds with
2016's blood (its spray effect, splatter decal and pool, at the game's most used values).

```
{
	type		wound
	kind		big			// small, big, headshot or limb
	parts		{ chest, left_upper_arm }	// the damage groups that open it
	opens		200			// the damage it takes in all
	inonehit	50			// and in one frame
	shows		wound_chest		// the mesh kit that appears
	cuts		{ { 0, 4, 60, 12 } }	// spheres cut from the body: { x, y, z, radius }
	blood		both			// spray, pool, both or none
	at		gorefx_chest		// the joint the blood (and what it throws) comes from
	throws		entities/gibs/chest_chunk
	whilealive	yes			// opens while the demon lives
	ragdoll		no			// yes: the demon ragdolls at once when it opens
}
```

`parts` is required; `at` is required unless `blood none`; `blood` left out is `spray`.

The demon's gore graph names its wounds, as for any wound.

## Mistakes

Run the Snapmap+ installer to update a mod whose material uses an earlier format. Previously published maps keep their original files and Snapmap+ reads those materials when the map opens.

The build reads every typed material, effect, environment and wound and stops on a mistake with one sentence,
for example:

- `blast.decl line 4: 'brigthness' is not a field of a light; the nearest is 'brightness'.`
- `blast.decl line 3: 'lihgt' is not an item of an effect; the nearest is 'light'.`
- `blast.decl line 3: a decal needs its 'material', as in 'material native("textures/decals/blood_splatter/blood_splatter_01")'.`
- `blast.decl line 3: 'pulse' of 'brightness' needs both 'low' and 'high', as in 'pulse { rate 2, low 0.2, high 1 }'.`
- `hell_red.decl line 3: 'fgo' is not a field of an environment; the nearest is 'fog'.`
- `rune.decl line 2: a material has no start, so 'bloomcolor' cannot fade once; write pulse, flicker, or a curve with 'loop yes', or fade the entity from a script.`

For a typed material, for example:

- `body.decl line 7: 'layer5' is not a field of a layered material; a surface has layers 1 to 4.`
- `embers.decl line 4: 'lighting' is 'glowing'; write unlit, lit or explosion.`
- `body.decl line 9: 'layer2repeat' needs one number, as in 'layer2repeat 3'.`
- `body.decl line 3: 'layer1nromal' is not a field of a layered material; the nearest is 'layer1normal'.`
- `body.decl line 3: 'wounds' is 'maybe'; write yes or no.`
- `body.decl line 3: 'type' must be the first key of the material; move it to the top.`

Also a mistake: a `type` other than `layered` or `particle`, a field of the other type (`lighting` in a layered material),
and a field written in the wrong shape (a list where one number goes, three numbers where an image goes). A key that is
not a field of the type and not close to one is left alone: it is the base material's own.

Nothing about the game's limits is ever reported: the product fits every material into them.

## Scripts

| Call | Does |
|---|---|
| `play_effect(effect, pos, [dir], [options])` | plays one of the mod's [effects](#effects) at a point, facing `dir`; `options` `{ colour = { r, g, b } }` tints this play. When the game holds nearly all the entities it can, the effect is skipped (one line in the log), never an error |
| `play_effect(effect, demon, [joint], [options])` | plays it on an entity, following it (on one of its joints when named) |
| `play_effect(effect, pos, [dir])`, `move_effect(e, pos, [dir])`, `stop_effect(e)` | the same for one of the mod's effect entities (an entity with an fx decl), as before; `move_effect` and `stop_effect` move and stop either |
| `set_environment(name)`, `clear_environment()` | lays one of the mod's [environments](#environments) over the players' views, and takes it off, each over its `blend` |
| `demon:recolour{ primary = {r,g,b}, secondary = ..., pattern = { {r,g,b}, {r,g,b} }, scratches = ..., dirt = ..., glow = ..., seconds = s }` | recolours the game's colour regions of an entity whose surfaces have them (the multiplayer armour and weapon materials, and a mod's materials on the game's `skinvmtr_custom` program), fading over `seconds`; one model drawn in many colours |
| `play_effect(e, condition)`, `stop_effect(e, condition)` | start or stop an entity's own fx |
| `e:tint(r, g, b, [seconds])` | washes the entity's surfaces toward a colour (`1, 1, 1` is none), fading over `seconds`; the game has no per-entity colour multiply, so a tint is a wash added over the lit surface, as the game's own highlights are |
| `e:glow(r, g, b, [seconds], [strength])` | makes the entity's glowing parts and surfaces glow in that colour (`0, 0, 0` is none), fading over `seconds`; the glow added is the colour times `strength` (0 to 16). Without a strength it is a soft glow (0.08) that leaves the surface readable under it; `strength` 1 is the full glow, which washes the whole surface with the colour (`e:glow(0, 0.6, 1, 0.5, 1)` paints a demon flat cyan). Pass `nil` for `seconds` to give a strength without a fade |

Both fade with the game's own render-parm fade (the `lerpRenderParmFrom` event), so a fade costs the script nothing
per frame. Other effects on one entity (a light at it, a decal under it) are fx: put them in an fx decl and play it
at the entity's position with `play_effect(effect, pos)`.

## For converters

Doomforge's adapters write materials in this same form: a converted demon's layered and particle materials are typed
materials, so a hand-made mod and a converted one use one surface.

## How it is drawn (for maintainers)

- A hair card is written with the game's own keys on its hair programs (`hairnew` and the pre-pass, shadow and blended
  edge beside it). The product's variants `snapmapplus/hair/hairprez`, `hairshadow` and `hairblending` are those
  programs with the death fade the skin programs have, so the card dissolves with the body its demon burns away; a
  material that names them in place of the game's three needs nothing else. See
  [The hair programs](architecture.md#the-hair-programs).
- `type layered` compiles to `snapmapplus/layered/skinvmtr` or `skinvmtrwounds` (the game's lit skinned programs with
  the layer blend added); its numbers go into a small per-material parameter image the product derives, so the
  program takes no render-parm slot of the engine's 1,024. A `type particle` material that needs a platform
  program uses `snapmapplus/particles/uberparticles` or `snapmapplus/particles-volume/uberparticles`.
  The SDK selects the volume variant for far fade or for a lit particle with a second glow-mask colour. Both use the game's existing particle keys. See
  [Render programs](architecture.md#render-programs).
- Effects, particles, environments, wounds and values that change are compiled by `src/backend/sdk.c` and the files it calls (`modeffect.c`, `modenv.c`, `modwound.c`, `sdkvalues.c`); what they derive (particle decls, materials, tables, the fx an environment plays, an effect's emitter) is served by the product, and the build carries it as pointers. See [The Graphics SDK: effects, environments, values that change](architecture.md#the-graphics-sdk-effects-environments-values-that-change).
- The compiler is `src/backend/modlook.c`; the build validates with it and the server serves its output. A material without a `type` is written in the game's own keys and is served as written. A material that compiles to the platform's layered program
  gets `packedspec 0` unless the material writes its own (the program reads the base pages unpacked).
