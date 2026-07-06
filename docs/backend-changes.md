# Backend changes — engine-call bugfix log

A running log of correctness bugs found and fixed in `src/backend/` — the shared engine-call layer
used by **both** the Qt frontend and the [experimental webview UI](webview-ui.md). These are cases
where our own reimplementation was wrong, not the original SnapHak's behavior; a divergence from
(or faithful reproduction of) the *original's* behavior belongs in [`fidelity.md`](fidelity.md)
instead. Entries are chronological, newest first.

## 2026-07-06 — `apply_engine.c`: prefab create-from-selection crashes

Two independent bugs, both in the `+0xb0` serialize-selection path (`slot_serialize_selection`),
found back to back while root-causing a hard DOOM crash on every "Create from selection" call. This
backend path had never been exercised by either frontend before the webview UI's first real call
into it — the Qt Prefabs tab has always been a "Coming soon" stub (see `sh_tabs.cpp`).

### 1. `PREFAB_TEMP_SIZE` undersized — stack buffer overflow

`PREFAB_TEMP_SIZE` (the scratch buffer for the temp `idSnapEntityPrefab`) was `0x220`, sized off the
original's `local_6d8` frame slot. Too small: the ctor at `+0x54d0a0` writes its own fields up to
`~+0x118`, then makes a small forward call into a second, larger ctor that keeps writing fields past
`+0x590` — the real object needs at least `~0x590+` bytes, about 2.6x the old allocation.

Every call overflowed the stack buffer. Because the overwritten bytes land on valid, mapped stack
memory (just not memory meant for this object), it was never a clean access violation, so neither
the fault-shield VEH nor our own SEH guard ever caught it — that's why it crashed DOOM outright
instead of failing gracefully.

**Fix:** bumped `PREFAB_TEMP_SIZE` to `0x2000` for comfortable headroom over the confirmed-required
size.

### 2. `PrefabPopulate` called with 2 args instead of 3 — uninitialized out-param

With the overflow fixed, a second, intermittent crash remained: two distinct locations inside the
engine's `populate()` function (base `+0x54e410`), both a `c0000005 ACCESS_VIOLATION` writing to
near-null address `0x10`, both caught cleanly by the fault-shield (recovers by exiting the editor to
the menu, not crashing DOOM outright): `+0x54e6e7` (`populate()+0x2D7`) and `+0x54f2a1`
(`populate()+0xE91`).

Root cause: `populate()` is actually a **3-argument** function — the 3rd (`R8`, per the Windows x64
calling convention) is an out `int*` status/reason code the engine writes through — but
`apply_engine.c`'s `prefab_populate_fn` typedef and call site only ever supplied 2 args. `R8` held
whatever was left over from the prior call in the sequence (garbage/unmapped, e.g. observed `0x10`),
so the engine's own write through it faulted.

This initially looked hover-state-dependent (an early, wrong theory pinned it on selection
size/complexity instead). That correlation was real but coincidental to the crash: whatever
DOOM's own hover-detection code happens to leave sitting in `R8` beforehand, not anything
`populate()` itself was reading for hover/placement purposes.

**Fix:** added the missing `int *outStatus` parameter to the `prefab_populate_fn` typedef and the
call site, passing a real local variable's address so the write always lands somewhere harmless.

### A confirmed, separate finding: the hover requirement is real

Fixing bug 2 surfaced the engine's *own* validation, previously masked by the crash: status code `2`
means "not hovering entity in selection," and the engine prints that exact message itself
(`"Failed to create prefab: not hovering entity in selection."`) before returning. Create-from-selection
genuinely requires hovering a selected entity in the 3D view — see
[`fidelity.md`](fidelity.md#create-from-selection-requires-hovering-a-selected-entity) — this is not
a clone bug. The webview UI now checks the hovered-id slot (`+0x198`) up front (see
[`webview-ui.md`](webview-ui.md)) instead of relying on the engine's status code, so it can show an
accurate message instead of a generic "nothing selected" one.
