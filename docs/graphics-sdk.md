# The Graphics SDK

A mod describes how its things look in plain terms; Snapmap+ turns the description into DOOM 2016's own material
and effect data. Where one of the game's own render programs draws the picture, the game's program is used. Where
none can, one of Snapmap+'s two general programs is used, the layered surface or the particle shading. An author
never names a program, a render parm, a texture slot or a renderer, and a material never fails to load because of a
limit: the product packs and budgets it, and reports only a mistake in the description, in one sentence that says
what to write instead.

The SDK has three parts:

- **Material types in data**: a material file whose first key is `type`. This guide covers the two types that need more than
  the game's own material keys: `type layered` (a surface built from tiled layers) and `type particle` (a particle
  stage's shading). Any other material is written with the game's own keys, as before.
- **Effects in data**: the game's fx decls (particles, lights, decals, render-parm fades, screen shakes, ribbons,
  flares), written as the game writes them and played with the script calls below.
- **Script calls**: `play_effect`, `move_effect`, `stop_effect`, `end_effects_at_once` and the entity calls `tint` and `glow`
  (see [Mod scripts](architecture.md#mod-scripts-entity-animation-and-world-calls)).

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
black specular or glow (the build log says which). Of the 190 layered and burning materials of DOOM Eternal's demons,
one asks for more.

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
| `glowmaskstrength` | value | 0 | the glow's strength (`lit`) |
| `gradient` | `{ r, g, b, r, g, b }` | white to white | the fire's colours along the glow mask (`explosion`) |
| `nearfade` | `{ begin, end }` | none | fades the particle out between two distances from the viewer (units) |
| `softdistance` | value | none | fades the particle where it meets a surface, over this many units |
| `depthoffset` | value | 0 | pushes the soft edge into thin parts of the image |
| `litscale` | value | 1 | scales the light a `lit` particle takes |

A particle material that sets none of `glowmask`, `gradient`, `nearfade`, `softdistance` and `depthoffset`, that is not
`explosion`, and (when `lit`) neither scales the light nor glows on its own (`litscale` other than 1, `glowmaskstrength`
other than 0), is drawn with the game's own particle shading (lit or unlit), exactly as a stock particle material is;
its `glow` is then the material's own brightness.

## Mistakes

The build reads every material of a type and stops on a mistake with one sentence, for example:

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
| `play_effect(effect, pos, [dir])`, `move_effect(e, pos, [dir])`, `stop_effect(e)` | play one of the mod's fx decls at a point (lights, decals, particles, shakes: whatever the fx holds) |
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

- `type layered` compiles to `snapmapplus/layered/skinvmtr` or `skinvmtrwounds` (the game's lit skinned programs with
  the layer blend added); its numbers go into a small per-material parameter image the product derives, so the
  program takes no render-parm slot of the engine's 1,024. `type particle` compiles to `snapmapplus/particles/
  uberparticles`, whose numbers are kept in keys the game's own particle program already holds. See
  [Render programs](architecture.md#render-programs).
- The compiler is `src/backend/modlook.c`; the build validates with it and the server serves its output. A material without a `type` is written in the game's own keys and is served as written. A material that compiles to the platform's layered program
  gets `packedspec 0` unless the material writes its own (the program reads the base pages unpacked).
