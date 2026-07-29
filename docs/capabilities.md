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
| `sh_user_overrides [0\|1]` | On a successful save, persist whether player override files load on the next DOOM launch. `0` disables and `1` enables only the first resource layer; restart DOOM to apply either value. If saving fails, the console says so, this launch stays unchanged, and no next-launch change is guaranteed. With no argument, reports this launch's state and either the saved next-launch state or a volatile value not confirmed saved. Built-in defaults and DOOM's packaged resources remain available. |
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
| Window shell | The Win32 host window + the manual 30 Hz think-loop; a menu bar with a persistent light/dark theme toggle (seeded before the page is first shown, so a saved dark theme never flashes light); the always-visible Camera-Origin bar (X/Y/Z track the live editor camera; "Lock Position" pins it). |
| Entities tab | A filterable entity list (multi-select, a persistent Show Hidden toggle, and one persistent selection direction: Follow Selection or Select in 3D; right-click for Copy ID / Delete / Push to stack 0 / Clear stack 0) plus the Entity State panel: classname / inherit / displayname fields and the Decl Text editor — line numbers, syntax coloring, structural lint, advisory schema checks, a distraction-free focus mode. "Save to Decl" commits the edits in memory. SnapMap's own built-in filter/droppable helper entities are excluded from the list and from every entity picker (dev-layer-only; a mapper's own filters are unaffected). |
| Keyboard paging | ArrowUp/ArrowDown page the Entities, Timelines and Prefabs lists, and — while their dropdown is open — the Inherit / Classname combos and the Timelines "Runs on" picker. Filtering still works alongside it; text editors and rename fields keep normal caret movement. |
| Native selection parity ("Select in 3D") | Entities selected from the list behave exactly like ones clicked in the 3D view: an empty-space click deselects them, Delete removes all of them, Move works, and the bottom-bar controls apply — single or multi-entity, including switching between entities. Deselecting natively also clears the list highlight. Previously only the explicit Deselect button worked, and Delete/Move misbehaved (Move could soft-lock the game); the cause was that the editor's own mode state was never told a selection existed. |
| Selection guard while holding | Selecting from the Entities list is refused (with an explanatory toast) while you're grabbing an entity or holding a staged prefab. The engine's Escape/cancel path restores a snapshot keyed positionally to the selection that was live when the grab began, so changing the selection first makes Escape swap entity pointers inside the live map — duplicating entities, deleting others outright, and freezing the game. A pre-existing engine bug (reproduces on released builds); only cancellation triggers it, never accept. Placing a *new* palette entity captures no snapshot and is left unrestricted. |
| List-assembled group grab | With a selection pushed from the Entities list, grabbing any one of those entities in the 3D view grabs **all** of them — not previously possible. Practical use: browse a logic chain in the 3D view, add the other entities you want to bring along from the Entities list, then grab the node you're on and move the whole group. You can also hold a grabbed entity or a staged prefab, push a selection from the list, and place the held object without losing the selection. |
| Prefabs tab | Save and load selection prefabs as JSON files under `%LOCALAPPDATA%\snapmap-plus\prefabs\` — one folder level with rename/delete/drag-between-folders; per-prefab description + tags (stored in a `<name>.meta.json` sidecar; the filter box matches tags across folders); "Load / Place" stages the prefab **and picks it up for you** — no Ctrl+V needed; you position it and click to drop, exactly as after a vanilla Ctrl+V. It does this by injecting the editor's own paste action, so the engine runs its real paste path rather than anything reimplemented. It degrades to stage-only (with a toast saying why) whenever the engine itself would refuse — not in EntityMode, already holding something, hovering an entity, or a selection that could not be cleared. A staged prefab also **survives a Play round-trip and a map change** — come back and Ctrl+V still works, matching the engine's own Ctrl+C clipboard. (Both were previously broken: the prefab had to be discarded on the way into Play to avoid a crash, because its entity-blob array was allocated in the engine's map heap, which is `HeapDestroy`d at map load. See [`backend-changes.md`](backend-changes.md).) |
| Timelines tab | The list of timeline entities; opening one edits its events and per-event parameters, with reference/decl/enum parameters constrained to valid choices, entity pickers for entity-typed args, and per-event documentation. |
| Feedback ("?") | The "?" button at the statusbar's right edge opens the Send-feedback dialog: category (bug / feature / incorrect info / other), title, details, optional contact. Sending files it as a labeled issue on this repo's tracker — no GitHub account needed. See the network note below + [`feedback.md`](feedback.md). |
| Crash reports | When the game hits a serious fault, a crash record is saved locally and the crash-report dialog auto-opens (in-session for a survived fault, next launch otherwise): the error + call stack, an optional description, and an opt-out checkbox to attach anonymized log tails. Sending files a `crash`-labeled issue; repeat crashes at the same location group onto one issue. See [`feedback.md`](feedback.md). |

## Persistent settings

`%LOCALAPPDATA%\snapmap-plus\config.json` holds player preferences shared through the backend-owned
settings service. The registered settings are `theme` (`"light"` by default, or `"dark"`),
`entities.show_hidden` (off by default), and `entities.selection_mode` (off by default; `off`, `follow`,
or `select_in_3d`), and `overrides.user_enabled` (on by default). Show Hidden and the exclusive Entity
selection direction survive restarts. The runtime creates the file when needed, so deleting it resets
preferences and the next startup recreates the defaults, including user overrides enabled. Manual config
edits are consumed at the next startup; a successful `sh_user_overrides 0` or `sh_user_overrides 1` save
uses the existing settings setter and recreates a deleted file. If the command cannot save, it reports the
failure, leaves this launch unchanged, and does not establish a next-launch change. The generic bridge already
permits a future frontend control for this setting.

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
| Rawmap save | On every editor save, mirror the serialized map JSON to `%LOCALAPPDATA%\snapmap-plus\rawmap.json`. |
| Overrides file-shadow | Three-layer resource resolution: your file under `%LOCALAPPDATA%\snapmap-plus\overrides\` wins when the player-file layer is enabled, then the built-in default decls (served from memory — the "*Custom" tab set: **Timeline**, **Unknown** and **Lift**; never written to your folder, so they update with each release and deleting your file restores the default), then the game's packaged resource. At startup, `overrides.user_enabled` is captured as an immutable choice for the player-file first layer; a successful `sh_user_overrides 0` or `sh_user_overrides 1` save changes the next launch and requires a DOOM restart. On a save failure, the console reports it and this launch remains unchanged. Built-ins and packaged resources remain available. Install logs an audit of your active overrides and reclaims untouched default copies written by earlier releases. |
| Strids injector | Inject custom `#str_` strings from `%LOCALAPPDATA%\snapmap-plus\strings\strids.json` into the engine string table (your key wins over a built-in default for the same id). |
| Text-field clipboard | **Ctrl+C / Ctrl+V in the editor's text fields** — every free-text property (datapad and transmission message bodies) plus the text-backed int / float / vec3 / size inspectors. Copies the selection, or the whole field when nothing is selected; pastes over the selection, or at the caret. Multi-line text pasted into a single-line field collapses each break to a space; datapad bodies keep their newlines. Respects the field's own character cap. Vanilla has no text clipboard at all here (only the dev console accepts a paste) — and it is genuinely absent rather than disabled: the engine's SWF text-edit key handler has no Ctrl branch whatsoever. Entity-level Ctrl+C/Ctrl+V and Ctrl+X are untouched. Exceeds the original. |
| Fault handling | The clone replaces the original's two kill-detours (which terminate DOOM) with the recover-in-place fault-shield (see [`fidelity.md`](fidelity.md)). |
| SuperScript table | Merge the parked `cs_*` SuperScript objects into the engine's eventDef enumerate/lookup/dispatch paths. |
| Math acceleration | An optional SIMD/threading accelerator for engine math; not required for parity (a perf feature, not a user-facing one). |

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
