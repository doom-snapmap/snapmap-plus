# Capabilities

What the clone does, grouped by kind. This is the user-facing feature inventory; for how the pieces
fit together see [`architecture.md`](architecture.md), and for the intentionally-faithful quirks
see [`fidelity.md`](fidelity.md).

## Console commands

Registered with the engine command system; run from the DOOM console.

| Command | What it does |
|---|---|
| `sh_rawmaps_on` | Enable raw map save/load (the `rawmap.json` substitution). |
| `sh_rawmaps_off` | Disable raw map save/load. |
| `sh` | The SnapStack dispatcher — routes the 20 SnapStack subcommands (below). |
| `sh_spawn` | Spawn `<entitydef> <name>` at the player and teleport to it. |
| `sh_dumpdef` | Print (and copy to clipboard) an existing entity's resolved entity definition. |
| `sh_spawninfo` | Generate `spawnOrientation` / `spawnPosition` from the current map position. |
| `sh_entlist` | List the editor entities. |
| `sh_listres` | List all resources of a given class (optionally copy to clipboard). |
| `sh_type` | Print the members of an idTech class or the values of an enum (runtime introspection). Add `-v` to also show each field's byte offset and size. |
| `sh_validclasses` | List the engine-valid classnames for a given inherit (the same enumerator that feeds the Classname dropdown). |
| `sh_target_any` | Reveal / re-hide the campaign-only and normally-hidden placeable entity decls in the SnapMap editor palette. |
| `sh_dumpmap` | Dump the current generated `.map` from memory to `<game dir>\base\mapdumps\<name>.map` (debugging). Never overwrites an earlier dump. |
| `sh_genbmodel` | Generate a bmodel from a `.obj` / `.ase` / `.lwo`. |
| `sh_genmd6model` | Compile a `.md6model` into a `bmd6model`. |
| `sh_debugrender` | Renderer debug toggle (developer tool). |
| `sh_alginfo` | Report the math-acceleration status. |
| `sh_help` | List every Snapmap+ console command and cvar with its description in one place. |
| `sh_superscriptop` | Dump SuperScript / eventDef data (e.g. emit the eventDef table as a header). |
| `cs_dumpeventdefs` | Dump all eventDefs to a file. |
| `cs_fieldinfo` | Print field info for a type (developer tool). |
| `cs_dontuse` | Toggle the higher-precision engine-math overrides (a precision/perf tradeoff; off by default). |
| `cs_start_render_logging` | Set up the render-logging hook. |
| `sh_disable_devmode` | Turn developer features off while keeping Bethesda.net connectivity. |
| `sh_reenable_devmode` | Turn developer features back on. |
| `sh_user_overrides [0\|1]` | On a successful save, persist whether user file shadows and user new-decl registration run on the next DOOM launch. `0` disables and `1` enables both user mechanisms; restart DOOM to apply either value. If saving fails, the console says so, this launch stays unchanged, and no next-launch change is guaranteed. With no argument, reports this launch's state and either the saved next-launch state or a volatile value not confirmed saved. Built-in defaults and DOOM's packaged resources remain available. |
| `noClip` / `infiniteHealth` / `noPlayerDeath` / `noPlayerKill` / `noTarget` | The five player-cheat toggles for the local player: no-collision flight, infinite health, can't die, can't be killed, enemies ignore you. |

## Cvars

Console variables; defaults shown in parentheses.

| Cvar | What it does |
|---|---|
| `sh_copy_reslist_to_clipboard` (0) | Copy `sh_listres` output to the clipboard. |
| `sh_pretty_on` (0) | Pretty-print saved rawmap JSON. |
| `sh_show_rmcount` (0) | Draw the current number of active rendermodels. |
| `cs_dash_direction_multiplier` (1.0) | Scale dash direction. |
| `cs_dash_ground_velocity_multiplier` (2.0) | Scale dash direction when on the ground. |
| `cs_dash_time_seconds` (0.5) | Time period over which the dash is applied. |
| `cs_num_dash_slices` (120) | Number of slices used to apply dash velocity. |
| `cs_mh_direction_multiplier` (1.0) | Scale meathook direction. |
| `cs_mh_movement_multiplier` (10.0) | Scale meathook velocity. |

## SnapStack ops

Subcommands of `sh`, operating on numbered entity-id stacks and named groups. A numbered stack
is one-shot scratch — apply/use ops drain it; move a set into a named group (`pop2g`) to reuse it.

The SnapStack stores + all 20 handlers live in the **backend** (`src/backend/snapstack.c` +
`json_patch.c`) — the sole implementation, registered once before the frontend loads. See
[`backend-changes.md`](backend-changes.md) for the port's design + the deferred-apply/`json_patch` fixes.

**The 20 OG subcommands** (usage `sh <op> <stack> …`; a numbered stack index; several ops also accept a
letter-first **group name** in place of the stack index):

| Op | What it does |
|---|---|
| `psel` | Push the current editor selection onto the stack, then clear the selection. |
| `popsel` | Add the stacked ids (or a named group's) back into the editor selection. |
| `phov` | Push the hovered entity onto the stack (de-duplicated). |
| `pr` | Push every valid id in an inclusive `[lo..hi]` range. |
| `pg` | Push a named group's ids onto the stack. `sh pg <grp>` alone implies stack 0. |
| `pop2g` | Move the stack into a named group. `sh pop2g <grp>` alone implies stack 0. |
| `cstk` | Empty the stack. |
| `filtinh` | Keep only the stacked ids whose inherit matches the argument. |
| `filtcls` | Keep only the stacked ids whose classname matches the argument. |
| `bss` | Bulk-set a string property on the stacked entities. |
| `bsi` | Bulk-set an integer property. |
| `bsf` | Bulk-set a float property. |
| `bsb` | Bulk-set a boolean property. |
| `bse` | Pop the last id; set each remaining id's chosen property to that id's id-string. |
| `bsin` | Set the inherit of each stacked entity. |
| `bscls` | Set the classname of each stacked entity. |
| `bsincls` | Set the inherit, then the classname, of each stacked entity. |
| `accl` | Pop the last id as the receiver; reference-assign the remaining ids to it. |
| `acctargets` | Pop the last id; append the remaining ids to its `targets` list. |
| `mkcmd` | Synthesize a reusable command-entity macro from the stacked ids. |

**New SnapStack+ commands** (clone additions — not part of OG's 20). They read and manage the same
backend stores every SnapStack op runs against, so they always reflect live stack/group state. Output
goes to the `~` console with a summary toast.

| Op | What it does |
|---|---|
| `chkstk [N]` | Inspect stack `N` (ids + id-strings); omit `N` to summarize every non-empty stack. |
| `chkgrp [name]` | Inspect a group's ids; omit `name` to list all groups + counts. |
| `clrgrp <name>\|*` | Delete a named group entirely (`*` deletes all). |
| `snapstack_diag` | Report, per subcommand, which loaded DLL currently owns the handler. |

## The Studio window

The Snapmap+ window — HTML/CSS/JS rendered in a WebView2 control, opened inside the SnapMap
editor (run `sh` in the console if it doesn't auto-open). Full detail: [`webview-ui.md`](webview-ui.md).

| Surface | What it does |
|---|---|
| Window shell | The Win32 host window + the manual 30 Hz think-loop; native Windows 11 rounded corners and drop shadow retained around the custom captionless chrome through the same one-pixel DWM frame treatment as snapmap-midi; a menu bar with a persistent light/dark theme toggle (seeded before the page is first shown, so a saved dark theme never flashes light); and three footer groups for the connection dot/version plus selection, the live read-only camera X/Y/Z origin, and Updated/help. The green dot is the sole visible connection indicator. Entities, Prefabs, and Timelines share one adjustable, keyboard-accessible vertical-divider position, equal 40px header tracks, common pane insets, and inset header/toolbar separators while their background fills remain continuous; the Assets tab is exempt and retains its purpose-built three-column layout without scrubbers. At compact widths the camera group reflows onto a second row, while the page remains clamped to the WebView client area so header and footer never scroll out of the viewport. Expanded Decl Text uses a client-bounded modal and dims the stationary shell underneath. Camera editing and locking are intentionally absent from the current UI, while their native bridge remains available for a future control. |
| Entities tab | A filterable entity list (multi-select, a persistent Show Hidden toggle, and one persistent selection direction: Follow Selection or Select in 3D; right-click for Copy ID / Delete / Push to stack 0 / Clear stack 0) plus the Entity State panel: classname / inherit / displayname fields and the Decl Text editor — line numbers, syntax coloring, structural lint, advisory schema checks, and a modal expanded mode that reparents the live editor without losing selection, scroll, or undo state. Diagnostic rows remain click-to-jump targets with pointer cursors but no hover highlight. Long entity rows ellipsize inside the same pane inset used by Entity State instead of painting into the divider, with the complete path/name available on hover. "Save to Decl" commits the edits in memory. Clicking a blank structural surface outside Entity State clears the current page and 3D-editor selection; the entire Entity State pane remains protected inspection space, including its blank padding and expanded Decl Text mode. SnapMap's own built-in filter/droppable helper entities are excluded from the list and from every entity picker (dev-layer-only; a mapper's own filters are unaffected). |
| Keyboard paging | ArrowUp/ArrowDown page the Entities, Timelines and Prefabs lists, and — while their dropdown is open — the Inherit / Classname combos and the Timelines "Runs on" picker. Filtering still works alongside it; text editors and rename fields keep normal caret movement. In the asset browser, paging onto a **file** row also selects it (debounced, so holding the key down previews only the row you stop on); **folder** rows still need Enter/Space, since opening one replaces the whole list. |
| Native selection parity ("Select in 3D") | Entities selected from the list behave exactly like ones clicked in the 3D view: an empty-space click deselects them, Delete removes all of them, Move works, and the bottom-bar controls apply — single or multi-entity, including switching between entities. Deselecting natively also clears the list highlight. Previously only the now-removed explicit Deselect button worked, and Delete/Move misbehaved (Move could soft-lock the game); the cause was that the editor's own mode state was never told a selection existed. |
| Selection guard while holding | Selecting from the Entities list is refused (with an explanatory toast) while you're grabbing an entity or holding a staged prefab. The engine's Escape/cancel path restores a snapshot keyed positionally to the selection that was live when the grab began, so changing the selection first makes Escape swap entity pointers inside the live map — duplicating entities, deleting others outright, and freezing the game. A pre-existing engine bug (reproduces on released builds); only cancellation triggers it, never accept. Placing a *new* palette entity captures no snapshot and is left unrestricted. |
| List-assembled group grab | With a selection pushed from the Entities list, grabbing any one of those entities in the 3D view grabs **all** of them — not previously possible. Practical use: browse a logic chain in the 3D view, add the other entities you want to bring along from the Entities list, then grab the node you're on and move the whole group. You can also hold a grabbed entity or a staged prefab, push a selection from the list, and place the held object without losing the selection. |
| Prefabs tab | Save and load selection prefabs as JSON files under `%LOCALAPPDATA%\snapmap-plus\prefabs\` — one folder level with rename/delete/drag-between-folders; per-prefab description + tags (stored in a `<name>.meta.json` sidecar; the filter box matches tags across folders); compact Lucide Create-from-Selection and New-Folder actions; and blank-list-space deselection through the shared selection router while Prefab Details remains protected. Selecting a prefab reconstructs its saved local transforms in an interactive, orbitable 3D preview above Name. Sparse saved rotations and dimensions are composed over their identity and installed entityDef defaults, so rotated props and thin/non-uniform blocking boxes retain their in-game placement and size. Orbit/zoom/frame help is top-right, `N entity/entities` is bottom-right, and Name, Description, and Tags use the same field typography as Entity State. Geometry is decoded on demand from the user's installed resource containers with neutral lighting: multi-surface props and pickup-spawner contents use their real models; blocking shells, invisible triggers, decals, interactables, full-size logic hexagons, and half-size input/output circles and filter diamonds receive distinct scene treatment. Only unsupported solid models remain proxy boxes. The Cartesian floor grid extends beyond the scene and fades under a circular mask. No textures or game-resource bytes ship with or persist in Snapmap+. "Load / Place" stages the prefab **and picks it up for you** — no Ctrl+V needed; you position it and click to drop, exactly as after a vanilla Ctrl+V. It does this by injecting the editor's own paste action, so the engine runs its real paste path rather than anything reimplemented. It degrades to stage-only (with a toast saying why) whenever the engine itself would refuse — not in EntityMode, already holding something, hovering an entity, or a selection that could not be cleared. A staged prefab also **survives a Play round-trip and a map change** — come back and Ctrl+V still works, matching the engine's own Ctrl+C clipboard. (Both were previously broken: the prefab had to be discarded on the way into Play to avoid a crash, because its entity-blob array was allocated in the engine's map heap, which is `HeapDestroy`d at map load. See [`backend-changes.md`](backend-changes.md).) |
| Assets tab | The asset browser — the shipped game catalog, searchable, with a live preview pane, and one click to put an asset on the map. Full detail in the section below. Also reachable as a modal from the Entities tab ("Browse assets"), scoped to the entity you came from. |
| Timelines tab | The list of timeline entities; opening one edits its events and per-event parameters, with reference/decl/enum parameters constrained to valid choices, entity pickers for entity-typed args, and per-event documentation. Its disabled Create action is a compact primary Lucide plus (with the standard primary hover treatment ready when enabled), and blank list space closes the current timeline through the shared selection router while the Timeline editor remains protected. |
| Feedback (help icon) | The Lucide Circle Help button at the statusbar's right edge opens the Send-feedback dialog: category (bug / feature / incorrect info / other), title, details, optional contact. Sending files it as a labeled issue on this repo's tracker — no GitHub account needed. See the network note below + [`feedback.md`](feedback.md). |
| Crash reports | When the game hits a serious fault, a crash record is saved locally and the crash-report dialog auto-opens (in-session for a survived fault, next launch otherwise): the error + call stack, an optional description, and an opt-out checkbox to attach anonymized log tails. Sending files a `crash`-labeled issue; repeat crashes at the same location group onto one issue. See [`feedback.md`](feedback.md). |

## The asset browser (Assets tab)

Every category is enumerated **live from the game's own shipped containers** when it is first
selected — no pre-extraction step, no bundled asset list, and nothing shipped in this repo. The
browser requests only that category and any qualifier it needs (the atlas-only set for Materials or
the soundbank map for Sounds), then keeps at most two category catalogs -- enough for the Assets tab
and modal to retain independent current views. Older category names and folder nodes are evicted,
while their scalar rail counts remain. Unvisited categories do not cross the WebView bridge or
occupy the page's catalog cache. Only assets from `snap_gameresources` are listed, with sounds as the
deliberate exception (see below): SnapMap never mounts the base game's broader
`gameresources.resources`, so a base-game-only model resolves in the editor as a black cube.

The native side is demand-driven too. Its first request parses the installed resource indexes into
names and payload offsets, interns equal names in compact storage, drops base-game record classes no
browser route can use, and releases the complete index buffers. Wwise metadata is not read until
Sounds is selected; the XML is streamed for only event and bank tags, then reduced to one copy of
each retained name. The `.vmtr` name union waits for Materials and uses exact strings rather than
fixed-width rows. Preview payload bytes stay in the installed game files and only the selected record
is read.

| Category | What it holds |
|---|---|
| **Pinned** | The mapper's own shortlist, at the top of the rail. Any asset, of any type, starred from its row; one shared list rather than one per type, because "the things I am working with right now" is rarely all of one kind. Kept in `%LOCALAPPDATA%\snapmap-plus\pinned.json` — deliberately **not** in `config.json`, so a malformed pin list can only ever cost the pins (see [Persistent settings](#persistent-settings)). |
| Materials | Surfaces. Previewed as real pixels — see below. The **union** of `material` decls and `.vmtr` megatexture atlas rows: a material is addressable by name *or* by rectangle and neither set contains the other, so a decl-only list hid thousands of rows that are paintable via Virtual Mapping. A **Cross Platform Textures** filter narrows the list to the 224 megatexture rects hand-tested to render identically on PC, Xbox and PlayStation. |
| Images | The lower-level image records the materials sample. They preview directly through the same bounded BC1/BC3/BC7 container decoder used by non-atlased materials. |
| Models | Props: `.lwo`, the `md6Def` set, and the `discreteAnimation` set — the last of these being the breakable/gib models that a `breakable` decl names, which are indexed under their own decl type and were invisible to a `model`-only catalog. |
| **Modules** | The 232 `mega_blessed` palette modules — whole SnapMap rooms, placeable as a single entity that is both visible **and solid**. |
| **Brush models** | Every other baked `.bmodel`: the individual wall, floor and detail pieces those modules are assembled from. Render-only. |
| **Clip models** | The `cm` type (`.bcm` / `.lwo` / `.md6`) — collision shapes, appliable on their own. |
| Sounds | The **union** of `sound` decls and Wwise events, deduplicated case-insensitively. Neither set contains the other, and the event-only half is ~2,600 names including the generic SnapMap VO, so a decl-only list is missing thousands of sounds a mapper can hear in the editor. Base-game-only sounds are offered too, and they work. Filed by **soundbank** rather than by name — see below. |
| **Lights** | The light **materials**: the projection a light shines through, written as `lightMaterial`. Point vs spot is not the asset — it is which entity carries it, so it is the Create-as choice. |
| FX, Particles, Decal atlases, Entity defs | The remaining placeable decl types. |
| **Perks**, **SWF / Flash** | Reference-only. A perk is granted by an `idTarget_Command` entity and a `.swf` belongs to an entity that owns a screen; neither structure is worked out, so both are names to copy and wire by hand. They are listed under Reference rather than Placeable on purpose — a category under Placeable whose Apply button does nothing reads as a broken tool. |

**Previews are real, and cover the catalog.** A material's pixels are produced by locating its pages
in the shipped megatexture set and decoding them with **DOOM's own page decoder, called in-process**.
That decoder is a pure function — no renderer, no GPU, no virtual-texture state and no map residency
— so a preview does not depend on the loaded map having the material on screen, which is what makes
whole-catalog browsing possible at all. Materials with no atlas rect (roughly half) fall back to
reading the image out of the `.index`/`.resources` containers and decoding BC1/BC3/BC7 directly.
Rows in the Images category enter that same container path at the image record instead of requiring
a material decl first. The selected kind is preserved, so an Image bypasses VMTR and wins even when
the installed index also contains a same-named Material. Sounds are auditioned through the editor's
own preview path with working play/stop. Mega2 previews read only the selected cell's page id,
offset/size entry, and payload; full shard tables are never copied into the process. The preview
worker sleeps while idle, and both its decode scratch and completed encoded image are released when
no longer needed.

**Apply to selection** writes the asset into the selected entity's decl and commits immediately —
one entity at a time, since it patches the decl the editor has open. **New entity** authors a
one-entity prefab and stages it through the engine's own paste path, so it arrives held and ready to
place.

**What may be applied is decided per target class**, in one place, and each carrier is gated to the
classes that own the field it writes:

| Asset | Writes | Allowed on |
|---|---|---|
| Material (by name or Virtual Mapping) | `customMaterial` / `virtualmapping` | the render-capable classes: blocking volumes, triggers, props, movers, `idAI2*`, cap entities, dynamic SnapMap entities |
| Model · Brush model · Module · Clip model | `renderModelInfo.model` (+ `clipModelInfo.clipModelName`) | the same set, **plus** interactables — minus three whose model *is* the mechanic (`idInteractable_Obstacle_SnapDoor`, `idInteractable_WorldCache`, `idInteractable_EliteGuard_Coop`) |
| Sound | `sound` | speakers (`idSnapMapGameEntity_Speaker`, `idSpeaker*`) |
| Light material | `lightMaterial` | lights (`idSnapMapGameEntity_Light`, `idLight`) |
| Particle | `particleSystem` | emitters (`idSnapMapParticleEmitter`, `idParticleEmitter`) |
| FX | `fxDecl` | FX entities (`idVolume_ToggleableDamageOverTime`, `idLaserHazard`, `idDynamicStampEntity*`) |
| anything | — | never the player start (`idSnapMapGameEntity_ComboStart*`), and never a variable or a SnapMap action |

Materials and models deliberately share one list: anything that can wear a model can wear a surface,
and re-texturing a mover or making a trigger volume visible are ordinary techniques.

The gate keys on the entity's **class**, never on whether that class is placeable from the editor's
palette — those are different questions. `idSnapMapParticleEmitter` has no placeable palette entry at
all, yet Snapmap+ creates one by overriding the classname on `snapmaps/unknown`, so gating on
placeability would refuse the emitter this tool just made.

A refusal names the class and says what it lacks, rather than greying the button out silently.

A module is placed by writing **both** halves — the baked geometry into `renderModelInfo.model` and
its paired collision into `clipModelInfo.clipModelName`. The two live at different paths and pair
232-for-232; the browser derives the collision name for you. The def's inherited `CLIPMODEL_AUTO` is
left alone, because naming a clip model overrides the automatic derivation on its own.

A **light** is placed by picking the light material and then choosing Point light or Spotlight under
Create as. Only `lightMaterial` is written; the cone, the colour and the intensity come from the
inherited def. Applying a light material to a light already on the map replaces its existing value.
Three of the shipped light materials contain a literal **space** in the name (`lights/gaus
_slowpulse` and two more) and the unspaced forms do not exist, so they are listed verbatim — anything
that trims or splits on whitespace corrupts them.

**Sounds are filed by soundbank, not by name.** After the duplicate collapse nearly every sound name
is a flat `Play_something`, so a name-derived folder tree was one root folder of ~8,000 rows. The
Wwise `<SoundBank>` grouping is the only real structure the catalog has — 24 non-empty banks, none of
them enormous, with `doom_snapmaps` being the set SnapMap itself loads. An event listed in several
banks is filed under one home (a specific bank in preference to the always-loaded `doom_initial`), so
no sound appears twice. Search deliberately cuts **across** banks rather than within the open one.

**No Refresh control, by design.** The catalog is indexed once per process out of `.resources` files
that cannot change while the game is running, so re-fetching returned identical bytes; a type whose
names never arrived re-asks when it is selected, which is the only retry that was ever needed.

## Persistent settings

`%LOCALAPPDATA%\snapmap-plus\config.json` holds player preferences shared through the backend-owned
settings service. The registered settings are `theme` (`"light"` by default, or `"dark"`),
`entities.show_hidden` (off by default), and `entities.selection_mode` (off by default; `off`, `follow`,
or `select_in_3d`), and `overrides.user_enabled` (on by default for both user decl mechanisms). Show Hidden and the exclusive Entity
selection direction survive restarts. The runtime creates the file when needed, so deleting it resets
preferences and the next startup recreates the defaults, including user overrides enabled. Manual config
edits are consumed at the next startup; a successful `sh_user_overrides 0` or `sh_user_overrides 1` save
uses the existing settings setter and recreates a deleted file. If the command cannot save, it reports the
failure, leaves this launch unchanged, and does not establish a next-launch change. The generic bridge already
permits a future frontend control for this setting.

**Pinned assets live in their own file**, `%LOCALAPPDATA%\snapmap-plus\pinned.json`, not in
`config.json`. The settings file is all-or-nothing: a parse failure or a schema mismatch sends the
whole document to "damaged → restored defaults". Settings are a handful of validated scalars and can
afford that; pins are unbounded data the user grows themselves, and a malformed pin list has no
business being able to reset somebody's theme along with it. The frontend host moves the bytes and
does no parsing — shape and validation live in the UI, the only side that knows what a pin means — so
the worst a broken file can do is cost the pins. Writes go to a temporary file and are moved into
place, so an interrupted write cannot truncate the real one. A missing file simply means "no pins
yet"; deleting it clears the shortlist and nothing else.

The schema and registry are intentionally extensible: registered values are type-checked and repaired
individually, while unrecognized root and `settings` members survive normal rewrites. A damaged file is
backed up and replaced with defaults; a file from a newer schema is left untouched; and an I/O failure keeps
ordinary settings for the current session while warning that they were not saved. `sh_user_overrides` is
different: its launch snapshot remains unchanged after a failed write. The installer treats this file as
player data and preserves it across update, uninstall, and reinstall.

## Network use

The clone makes exactly **one** kind of network request, and only on an explicit user action: clicking
**Send** in the feedback dialog — or in the crash-report dialog — POSTs the typed report (category /
title / details / optional contact) plus the installed version string over HTTPS to the project's
feedback relay ([`feedback/`](../feedback/README.md)), which files it as a public issue on this repo's
tracker. A crash report can additionally attach the tails of the local log files, but only when its
"Attach recent logs" box is checked, and the text is anonymized first (account / profile / machine
names scrubbed — see [`feedback.md`](feedback.md)); the local crash dump is never uploaded. Nothing is
downloaded, nothing runs in the background, and nothing else is ever sent. (The installer
separately talks to GitHub Releases for `snapmap-plus update`, and to Microsoft's bootstrapper if the
WebView2 runtime is missing — both documented in [`packaging.md`](packaging.md).)

## Hook behaviors

Engine detours and resource-loader shadows the backend installs.

| Hook | What it does |
|---|---|
| Rawmap load | When rawmaps are on, load the map from `%LOCALAPPDATA%\snapmap-plus\rawmap.json` instead of the engine's own save. |
| Rawmap save | When rawmaps are on, mirror each saved map's JSON to `%LOCALAPPDATA%\snapmap-plus\rawmap.json`. Off, your save is untouched and that file is left alone — the same switch governs both directions. |
| Overrides file-shadow | Four-layer resource resolution: your loose file under `%LOCALAPPDATA%\snapmap-plus\overrides\` wins when the player-file layer is enabled; then an exact resource admitted by the read-only installed-resource bridge; then the built-in default decls (served from memory — the "*Custom" tab set: **Timeline**, **Unknown** and **Lift**); then the game's packaged resource. At startup, `overrides.user_enabled` is captured as an immutable choice for both user-owned layers; a successful `sh_user_overrides 0` or `sh_user_overrides 1` save changes the next launch and requires a DOOM restart. Built-ins and packaged resources remain available. The custom resource stream is enabled only on the audited 31-slot Steam-build ABI; incompatible builds refuse before publishing the hook. Install logs an audit of active loose files and reclaims untouched default copies written by earlier releases. |
| Installed resource bridge | A restart-only manifest under `%LOCALAPPDATA%\snapmap-plus\overrides\generated\resources\*.manifest` can name exact `(type, logical name, virtual path)` rows that already exist in the user's installed base-game `gameresources.pindex`. Multiple unique virtual paths may belong to one logical identity, which supports compiled shader and similar multi-file bundles. Byte-identical repeated pindex rows collapse deterministically; divergent repeats refuse the complete snapshot. Snapmap+ validates the complete manifest and archive bounds at startup, opens the installed archives read-only, and decodes only a requested slice into memory. It copies no game payload into the package and modifies no pindex, archive, sidecar, executable, or verification file. This exposes installed cut/campaign resources; it cannot supply a binary asset absent from that DOOM installation. |
| Dynamic decl server | Cold-start registration for genuinely new text decl identities from local files under `%LOCALAPPDATA%\snapmap-plus\overrides\generated\decls\<type>\...*.decl` plus linked game-owned `.decl` rows from the installed resource bridge. The first directory is the engine decl type; the remaining path without `.decl` is the logical name. A local same-identity file wins over a linked original. Discovery resolves all case-insensitive identity collisions before deterministically admitting up to 512 entries; unsafe paths, multiple top-level body blocks, unexpected enumeration/read failures, and ambiguous identities are refused before queueing. Uniquely resolved quoted dependencies establish deterministic component order; strongly connected cycle members retain stable admission order within their component. Once engine `load_state` reaches `RUNNING`, one main-thread command resolves each type through the registry and checks `DeclSourceFind(typeManager, logicalName)` first: an existing source record is immediately `SHADOWED` without a live lookup, while a null source falls back to lookup-only `DeclFind(..., makeDefault=0)`; a live object is also `SHADOWED`, unsupported types are `REFUSED`, and only both-null identities are `MISSING`. Snapmap+ copies each missing body's validated single brace block into an immutable process-lifetime exact table keyed by lower-case `decltree/<type>/<logical-name>.decl`, then constructs a native 48-byte `idStr` for each `<type>/<logical-name>.decl` and invokes the registry's native `+0x38` source scanner once per identity in dependency order. After all scans succeed, every missing identity is given a live object in its own manager in that same dependency order -- non-editor types first, then editor entities -- because DOOM's decl parsers resolve their own `inherit` and `edit.entityDef` edges with `makeDefault=0` and only find targets that already exist. Each identity is looked up with `DeclFind(..., makeDefault=0)`, which lazily loads a pending object, and only a genuinely absent object takes `makeDefault=1`; a null manager, null object, unreadable state byte at `+0x2c`, or in-progress object is terminal. Missing `snapEditorEntityDef` identities are additionally gated lexically from their captured bodies: only a real top-level `inherit = ...` or direct `edit.entityDef = ...` assignment is materialized at all; comments, quoted decoys, nested fields, and source-only abstract bodies are `NON-PALETTE`. An editor entity counts for the derived palette only when it satisfies the same contract DOOM's own palette validator applies: a non-null resolved entityDef at `+0x1c8`, every output target flagged `0x20`, and every input target flagged `0x10` at `+0x3cd`, with bounded target counts. The generic valid bit `0x04` is logged as a diagnostic and is not an admission condition. The provider serves matching table entries before ordinary layers; there is no aggregate alias, physical-file fallback for a published identity, alternate C-string call, raw-object cache, `DeclFind` detour, per-identity `AddFromText` call, arbitrary default synthesis, refresh, hot reload, retry, or unload. A source/live lookup exception, false result, invalid materialized decl, or constructor/scanner/destructor exception terminates registration while retaining the immutable table. |
| Package requirements | Restart-only files under `%LOCALAPPDATA%\snapmap-plus\overrides\generated\requirements\*.requirements` can request product-audited, idempotent cvar/value pairs. SnapMap+ validates the complete snapshot and queues admitted settings once only after engine `load_state` reaches `RUNNING`. The initial allowlist is `g_useResourceBlackList 0` and `g_useImageBlackList 0`; arbitrary commands and arbitrary cvars are refused. |
| Strids injector | Inject custom `#str_` strings from `%LOCALAPPDATA%\snapmap-plus\strings\strids.json` into the engine string table (your key wins over a built-in default for the same id). |
| Text-field clipboard | **Ctrl+C / Ctrl+V in the editor's text fields** — every free-text property (datapad and transmission message bodies) plus the text-backed int / float / vec3 / size inspectors. Copies the selection, or the whole field when nothing is selected; pastes over the selection, or at the caret. Multi-line text pasted into a single-line field collapses each break to a space; datapad bodies keep their newlines. Respects the field's own character cap. Vanilla has no text clipboard at all here (only the dev console accepts a paste) — and it is genuinely absent rather than disabled: the engine's SWF text-edit key handler has no Ctrl branch whatsoever. Entity-level Ctrl+C/Ctrl+V and Ctrl+X are untouched. Exceeds the original. |
| Fault handling | The clone replaces the original's two kill-detours (which terminate DOOM) with the recover-in-place fault-shield (see [`fidelity.md`](fidelity.md)). |
| SuperScript table | Merge the parked `cs_*` SuperScript objects into the engine's eventDef enumerate/lookup/dispatch paths. |
| Math acceleration | An optional SIMD/threading accelerator for engine math; not required for parity (a perf feature, not a user-facing one). |

After complete source registration and successful materialization of eligible
new `snapEditorEntityDef` objects (source-only abstract bodies are excluded by
the lexical gate), the main-thread registration command performs
one engine-owned palette rebuild against the validated live palette object.
The registration-success bit is published only after that rebuild returns
successfully. This is a derived-list refresh only: it does not alter rawmaps or
game archives. The builder runs once for either editor initialization state; a
materialization, object, vtable, native-call, or false-result refusal prevents
success.
Unsupported builds, invalid palette objects, and native exceptions refuse the
one-shot operation.

## Also in the backend

Beyond the manifest entries above, the backend `XINPUT1_3.dll` also bundles **cvar-unlock**
(developer cvars and `+<cvar>` launch options), a fixed **XInput proxy** (controller input keeps
working, connected or not), and the resident **fault-shield**. See [`README.md`](../README.md)
and [`fidelity.md`](fidelity.md).

## Not user-facing

A few internals are present for parity but are not features you drive directly:

- The 5 parked `cs_*` SuperScript objects (`cs_invert_activator`, `cs_flyto_target`,
  `cs_test_equipped_weapon`, `cs_clone_activator`, `cs_dash`) are disabled gameplay-cheat objects
  in this build; their only live surface is the `cs_*` cvars above.
- `snaphak_ext` (a VFS / shader-compile library) is dormant, and `snaphak_algo` (SIMD + threading)
  is an optional performance accelerator — neither is required to match the original's behavior.
