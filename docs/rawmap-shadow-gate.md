# Open decision — the rawmap save shadow shares one switch with the load swap

**Status: DEFERRED, 2026-08-10. No code change made.** This documents a reported behaviour change, what
was proven about its cause, what was ruled out, and the proposed fix — so the decision can be made later
without re-deriving any of it.

---

## The report

> "Doom Snapmap asking me to save my map like its a new map with a new name everytime when
> `sh_rawmaps_on` is not set. The behavior before was rawmap.json was always written on a save
> regardless of the setting of the command."

Two distinct observations, and they have different answers. One is ours and fully explained; the other
is not ours and is still open.

---

## Observation 1 — rawmap.json no longer written unless armed. EXPLAINED, ours.

Caused by [`7ede1e7`](https://github.com/doom-snapmap/snapmap-plus/commit/7ede1e7) *("the SAVE shadow now
obeys the same switch the LOAD swap does")*. Before it, `sh_ser_detour` mirrored every saved map to
`rawmap.json` unconditionally from the moment it installed. After it, both detours share one
`rawmap_armed()` predicate.

Confirmed in a user's own `sh_backend.log` across the build cutover:

```
20:27–21:00  B1: rawmap SAVE shadow wrote ... -> rawmap.json [#1..#4]   <- pre-7ede1e7, ungated
23:19        B1: rawmap LOAD-swap installed ...; gate=DISARMED          <- post-7ede1e7 build
23:35        gate=DISARMED
             (two subsequent in-game saves produced NO "wrote" line)
```

Upstream's rationale is sound and should not be dismissed: `rawmap.json` is the file you hand-stage a map
into before loading it, so an ungated shadow silently destroys deliberately-placed content on the next
save, with nothing in the log to say so.

## Observation 2 — SnapMap prompts to save as a new map. NOT OURS, still open.

Four paths could plausibly touch this. All four were checked and none can:

| Path | Finding |
|---|---|
| Save shadow (`sh_ser_detour`) | `g_ser_orig(...)` runs **unconditionally and first**; `rawmap_armed()` is checked only afterwards (`rawmap.c` ~404-415). The gate can only skip our disk copy — it cannot alter what the engine writes or which slot it targets. |
| Load swap (`sh_deser_detour`) | Disarmed path is `return g_deser_orig(json, out_map)` — a clean passthrough. |
| `sh_rawmaps_on` / `sh_rawmaps_off` | Handlers call `sh_rawmap_swap_arm()` and print. No save format, no dialog, no map identity. |
| New fault-shield map-load guards | `shield_faults.log` shows only `armed` lines (installed), never a fired/repair line. Inert. |

**Working hypothesis — NOT established, do not treat as fact.** The prompt itself probably did not
change; its *consequence* did. If the map has no DOOM map-slot identity — which it would not, if
`rawmap.json` *is* the working map — SnapMap has always prompted. Previously that prompt was ignorable
noise because the shadow had already mirrored `rawmap.json` regardless. With the shadow gated, completing
the prompt became the only way to get the map saved anywhere, so it went from background annoyance to
blocker.

**The one question that settles it:** before the gating change, did that prompt appear and simply get
cancelled/ignored — or did Save genuinely complete silently with no dialog at all? If the former, there
is no second bug and fixing Observation 1 resolves the whole report.

---

## The actual design problem

`7ede1e7` welded two operations with very different risk profiles onto one switch:

| Half | What arming it does | Risk |
|---|---|---|
| **LOAD swap** | Every map load is *replaced* by `rawmap.json` | **High** — silently substitutes your map. Must be explicit. |
| **SAVE shadow** | A copy of the saved map is mirrored to `rawmap.json` | **Low** — writes one file. Only harmful while hand-staging that file. |

The consequence is that you cannot have *"always mirror my saves"* without also accepting *"always hijack
my loads."* For a user whose workflow treats `rawmap.json` as their working map, the shadow is the
feature and the swap is the hazard — and the single switch forces them to take both or neither.

That coupling — not the gating itself — is the defect worth fixing.

## Proposed fix — split the switch

Give the shadow its own gate, defaulting **ON**, and leave the load swap gated exactly as upstream made
it. This restores the pre-`7ede1e7` save behaviour while keeping the dangerous half explicit, and it
*answers* upstream's data-loss concern rather than ignoring it: someone hand-staging `rawmap.json` is
precisely the person who knows to turn the shadow off first.

Implementation sketch (`src/backend/rawmap.c`, `src/backend/commands.c`):

1. Split `rawmap_armed()` into `rawmap_load_armed()` and `rawmap_shadow_armed()`.
2. Add `g_shadow_gate`, initialised to `1` (on). `sh_deser_detour` keeps using the load predicate;
   `sh_ser_detour` uses the shadow predicate.
3. Add `sh_rawmap_shadow_on` / `sh_rawmap_shadow_off` to the command table, mirroring the existing
   `sh_rawmaps_on`/`off` handler shape.
4. Keep `arm.flag` arming **both** — it is the no-console test trigger and should stay all-or-nothing.
5. Report the shadow gate state in the existing install log line alongside `gate=` for the swap.

### Alternatives considered

- **Revert `7ede1e7` outright** — gets the old behaviour back in one line, but reintroduces the silent
  overwrite upstream deliberately fixed, and puts a bare revert of another maintainer's commit in the
  tree. Rejected.
- **No code change; use `arm.flag`** — works today, but arming the flag also arms the LOAD swap, so it
  cannot be left on permanently without every map load being substituted. Adequate as a temporary
  workaround only, and only with `rawmap.json` backed up first.

### Coordination

`7ede1e7` is upstream's (`doom-snapmap/snapmap-plus`, adiazpar, 2026-08-07). Any change here should go
upstream as its own commit or PR with the coupled-switch argument above — **not** folded into an
unrelated feature branch, where it would read as a silent behaviour revert.

### Safety note for anyone testing this

Arming (`sh_rawmaps_on` or creating `%LOCALAPPDATA%\snapmap-plus\arm.flag`) arms the **load swap too**.
Back up `rawmap.json` before an armed save — it will be overwritten — and disarm afterwards.
