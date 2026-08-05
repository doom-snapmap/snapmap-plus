# Backend changes — engine-call bugfix log

A running log of correctness bugs found and fixed in `src/backend/` — the shared engine-call layer
behind the frontend. These are cases
where our own reimplementation was wrong, not the original SnapHak's behavior; a divergence from
(or faithful reproduction of) the *original's* behavior belongs in [`fidelity.md`](fidelity.md)
instead. Entries are chronological, newest first.

## 2026-08-05 — the last hardcoded engine RVAs are gone, and the `[64]` that hid a truncated signature DB bit again

**What changed.** Three engine functions were still reached as a raw `module_base + RVA`, which is
build-locked in exactly the way the signature resolver exists to avoid. All three are now ordinary
signature entries, verified unique across the whole executable image:

| Signature | Pinned RVA | What it is |
|---|---|---|
| `Mega2PageDecode` | `0x196E140` | the megatexture page decoder the asset browser calls |
| `PrefabDtor` | `0x51D870` | `idSnapEntityPrefab::~idSnapEntityPrefab` |
| `EntityDeshare` | `0x52C920` | COW make-unique, before an in-place entity edit |

The two prefab functions join the ctor/populate/`MemLocal` group on `ae_pick_engine_fn` — signature
wins, `known_rva` cross-checks and logs a mismatch loudly rather than silently preferring either.

The decoder previously carried a **local `memcmp` of its own prologue** at the hardcoded address.
That is strictly weaker than a signature and the difference is not academic: a byte check can detect
that a function moved but can never *find* it, so any build with a shifted `.text` lost previews
outright. It now resolves wherever the loader put it, and a build whose bytes genuinely differ fails
to resolve — which still disables previews, but by refusing rather than by calling into whatever now
occupies that address.

**One function could not be signatured, and is not pretending to be.** idList-grow (pinned
`0x699A60`) is **one of 1,560 byte-identical instantiations** of the same idList template in the
image, separable only by their displacements. No lengthening of a prologue pattern gets past that —
the same wall `StopSound`'s clone hit. It is resolved *relationally* instead: `AddCommand` (which is
signatured) calls it on `cmdSys+0x08` via `LEA RCX,[RSI+8]` / `CALL rel32`, so the callee is decoded
out of that call site, range-checked against the module, and logged with the pinned RVA as a
cross-check. The decode runs before our own `AddCommand` detour is installed, and the call site sits
well past the 15 stolen prologue bytes either way.

**The bug this uncovered.** `tests/sig_test.c` and `tests/hooktol_test.c` both still declared
`sig_result results[64]` — the same fixed array that silently truncated the database at entry 65 and
prompted `SIG_RESULTS_MAX` in the first place. The constant was applied to the shipping callers and
missed in the tests. At 67 entries it stopped being silent and became a stack overwrite:
`sig_resolve_all` wrote past the end of the array, producing garbage status values and an access
violation **inside the test process**. Both now size with `SIG_RESULTS_MAX` and print a loud
`SIGNATURE DB OVERFLOW` and fail if the database ever outgrows it.

The general lesson is worth keeping: a constant introduced to fix a class of bug only fixes the call
sites it is actually applied to. Grep for the literal, not just for the symptom.

**Verified.** `sig_test` 67/67 signatures resolve to their known RVAs; `hooktol_test` 3/3
hook-tolerant fallbacks; 16/16 native tests; `go test ./...` clean.

## 2026-07-28 — Load/Place now picks the prefab up (auto-grab enabled): the corruption was the heap bug

**What changed.** `kind=2` (stage → pick up) is enabled. Load/Place no longer asks the user to press Ctrl+V.

**Why it was off.** An unexplained `"Memory corruption before block!"` had followed two auto-grab pastes,
while repeated manual pastes looked clean, so auto-grab was implemented and left disabled. Two suspects were
on record, both states the engine never produces for itself: we **set** the paste-available bit rather than
letting the engine recompute it, and we **ClearSelection** immediately before arming.

**Why it is on now.** Re-tested after the map-heap fix below, with instrumentation:

- **Eight auto-grab pastes across two sessions** — including three on a fresh map followed by several
  minutes of ordinary editing — produced **no corruption**.
- **Post-paste editor state is identical to a manual Ctrl+V:**
  `mode+0x1ac=4  arm(+0x420)=-1  action=0x0  flags1(+0x41)=0x64 pasteAvail=1  flags2=0x00 dirty=0`.
  So neither suspect leaves any residue, and the arm word self-clears — an injected action cannot re-fire.
- The original corruption is therefore attributed to the **staged-prefab map-heap bug** fixed the same day
  (entry below), which was live at the time of every earlier auto-grab observation. Consistent with the fact
  that the "auto-grab specifically" reading rested on a manual control whose two recorded accounts
  contradicted each other.

**We do not bypass a capacity check.** Read from the engine's gate recompute, the paste-available bit is
*exactly* two conditions — the copy/paste cvar AND staged entity count ≥ 1 — inside the nothing-hovered
branch. **No budget term.** And because this route injects an action rather than calling `PasteInstantiate`
directly, the engine still runs its own paste branch and every check inside it. Separately confirmed: the
in-editor budget rows are **soft** (the editor deliberately allows exceeding them), so they are not something
to gate on either.

**The one honest residue:** we set the bit without reading `snapEdit_enableCopyPaste`, so a paste is possible
while the user has copy/paste disabled. Minor — the engine's next recompute clears the bit again.

**Degradation is unchanged and still load-bearing.** Any of "not in EntityMode", "already holding"
(`mode+0x1ac` is 4), "hovering an entity", or "selection could not be cleared" aborts to stage-only with a
toast. All were observed firing correctly during the test, including five consecutive `mode busy` refusals
while the editor was holding.

**Not fixed here, and unrelated:** under deliberately heavy load (several hundred entities pasted in ~2
minutes) the engine raised `"Cannot map buffer with usage BU_STATIC"`, showed a modal and returned to the
map-select menu. That is the engine's documented `level>=6` recoverable path, not corruption, and it was not
reproduced on the fresh-map run. `snapEdit_skipLimits` was **not** set. Cause unattributed; nothing links it
to this code path, and a vanilla session could plausibly reach the same state.

## 2026-07-28 — the staged prefab now survives Play and map changes: it was in the wrong heap

**Supersedes the "Known limitation" of the entry below.** That entry stopped the crash by discarding our
staged prefab on the way into Play. This one removes the need to discard it at all.

**Root cause.** Not structural inequivalence — the object we deserialize is member-for-member identical to
`CreatePrefab`'s. The difference is **which heap its entity-blob array is allocated from.** Read live from
each block's own allocator header:

| block | tag | flags | size | heap |
|---|---|---|---|---|
| engine Ctrl+C clipboard, 1 entity | `0x05` | `0x00` | 432 = `1 × 0x1B0` | **process heap** |
| our staged prefab, 5 entities | `0x05` | `0x04` | 2160 = `5 × 0x1B0` | **map heap** |

Same element type, same tag, different heap. The engine keeps three heaps — global, persist, map — and
`ResetMapHeap` calls `HeapDestroy` on the map heap at map load. So our array's pages were handed back to
the OS while the prefab's `idList` header still pointed at them with `num == capacity == 5`. Measured
directly: after Play the header was **byte-identical** but reading blob byte 0 **faulted**. Unmapped, not
freed — which is the distinction that identified the mechanism, since a freed block's pages stay committed
and read as garbage.

That also explains why the same bug produced two different symptoms. Touch it while still unmapped and you
get an access violation; touch it after unrelated allocations have re-committed that address range and you
get plausible garbage with the heap-owned bit set, which fails the allocator's guard-cookie check and
raises the fatal `Memory corruption before block!`.

**Why the allocation lands there.** Every idlib container asks `Mem_Alloc` for heap id `-1`, meaning
"whatever is on top of `idMemLocal`'s 32-deep heap-scope stack". An allocation's lifetime is therefore
decided by *when* it runs, not by what is allocated — and while the SnapMap editor is up the engine already
keeps the **map heap pushed** (observed scope depth 1). Our deserialize inherited it.

**Fix.** Wrap the stage in the engine's own scope API — `idMemLocal::PushHeap(0)` / `PopHeap()`, reached
through the instance returned by the allocator's magic-static getter. Heap-table slot 0 is `NULL` and
`Mem_Alloc` falls through to `GetProcessHeap()` when the selected handle is null, so pushing `0` puts the
blob array exactly where the engine's own clipboard lives. The engine uses this same `PushHeap(0)`/
`PopHeap()` idiom itself. **No member copying changed.**

`sh_apply_prefab_poll_play()` then stops discarding our prefab — but it asks the block rather than
assuming: `ae_block_survives_map()` validates the allocator's guard cookie and reads the owning heap out of
the block header, returning 0 for anything it cannot verify. So if the push ever stops working, the old
protective re-ctor runs and the user gets the previous behaviour rather than a crash.

**Verified.** Survives a Play round-trip *and* a map change, with Ctrl+V working afterwards — matching the
engine's own clipboard.

**Two things worth carrying forward.**

- *Balance matters.* `PopHeap` **fatals on underflow**, so push/pop are paired with `__try/__finally`.
- *The scope stack is global, not per-thread*, so this briefly changes the ambient heap for other threads.
  That is a **leak** risk, never corruption: every block records its own heap in its header and `Mem_Free`
  reads it back, so a block is always freed into the heap it came from regardless of the scope at free
  time. Also note the whole mechanism is **main-thread-only** — `PushHeap`, `PopHeap` and `Mem_Alloc`'s
  `-1` lookup share a `GetCurrentThreadId()` gate, and off that thread all of it is silently inert.

The three engine functions are resolved by **signature** (`MemLocalGet`, `MemLocalPushHeap`,
`MemLocalPopHeap`), not raw RVA, and each call is range-checked against the DOOM module before being made
— a wrong pointer here would be *called*, not merely read. An earlier revision used a hand-computed RVA
with a dropped digit, which put the object ~256 MB past the module and made the push a silent no-op.

Full engine-side derivation: doom-re `docs/truth/engine/memory-heaps-and-allocator.md`.

## 2026-07-28 — a staged prefab poisoned the paste slot and killed vanilla Ctrl+C / Ctrl+V after Play

**Symptom.** Stage a prefab with Load/Place, press Play, come back to the editor — and the game's own
copy and paste were dead. They stayed dead until you pressed Load/Place again, which appeared to "fix"
them. Vanilla copy/paste with no Load/Place involved was completely unaffected, which is what made this
look like an editor-state problem for a long time.

**What was actually happening.** Ctrl+C was not being refused — it was *running and faulting*. The
shield log showed repeated first-chance access violations at `rip+0x1AB32EE` reading `-1`, classified
`in-editor draw fault -> aborted draw, resumed editor frame`. The engine's copy handler calls
`CreatePrefab` on the shared staging slot at `editor+0x209a8`, and `CreatePrefab`'s first act is to tear
down whatever is already in that slot. Our staged prefab does not survive a Play round-trip, so that
teardown walked pointers the map teardown had already freed. Pressing Load/Place "fixed" it only because
`ae_mkcmd_one` re-ctors the slot before staging, which happens to heal it.

This is **pre-existing** and independent of any Load/Place automation work — it reproduces with plain
stage-only behaviour, and would hit anyone who staged a prefab and then played their map.

**Fix.** `sh_apply_prefab_poll_play()` (polled from the per-tick drain) watches the engine `load_state`
global and re-initialises the staging slot on the way **into** Play, while its memory is still intact.
Re-ctor'ing is the right tool: the prefab ctor rewrites every field unconditionally with no reads and no
frees, so it turns dangling pointers into a clean empty prefab — it leaks the already-dead allocations
rather than double-freeing them.

Two guards matter as much as the fix:

- **Ownership.** We only touch the slot when *we* staged it and the entity count is unchanged. An
  engine-made Ctrl+C clipboard is left strictly alone — it legitimately survives a Play round-trip and
  across maps, and an earlier revision of this fix that cleared the slot unconditionally destroyed the
  user's clipboard and disabled paste. Don't do that.
- **Direction.** Cleaning on the way *back* is too late; the engine can touch the dangling slot first.

**Detecting Play is harder than it looks.** Two obvious signals do **not** work and should not be
retried: the editor session going null (`editor+0x204c8`), and the loaded-map object pointer changing.
Both survive a Play round-trip, so neither ever fired. The engine's `load_state` global
(`base+0x6DDE198`; `3` = RUNNING) does move, and per the fault-shield's own notes an in-editor in-place
load never writes it.

**Known limitation.** ~~Our staged prefab still does not survive a Play~~ — **RESOLVED later the same
day, see the entry above.** The cause was not structural inequivalence at all: the object we build is
member-for-member identical to `CreatePrefab`'s. It was allocated in the wrong **heap**. Note the
hypothesis recorded here — "make `ae_deserialize_to_obj` build an object structurally equivalent to what
`CreatePrefab` produces" — was **wrong**, and chasing it cost several rounds; the members already matched.

## 2026-07-13/14 — SnapStack lives in the backend (`snapstack.c` + `json_patch.c`); the `json_patch` empty-`edit` fix; store slots + management commands

**What & why.** The SnapStack subsystem — the stack-of-stacks + named-group stores and all 20 `sh`
subcommand handlers (`psel`/`popsel`/`phov`/`pr`/`pg`/`pop2g`/`cstk`/`filtinh`/`filtcls`/`bss`/`bsi`/`bsf`/
`bsb`/`bse`/`bsin`/`bscls`/`bsincls`/`accl`/`acctargets`/`mkcmd`) — is hosted **in the backend**, in pure C
(`src/backend/snapstack.c`), backed by a purpose-built dotted-path JSON mutator
(`json_patch.c`) that generalizes `apply_engine.c`'s existing raw-splice technique instead of pulling in a
JSON library (the backend deliberately carries none). Registered from `ui_bridge.c` before the frontend
loads: **one implementation, one store**, and every `kind=0` decl-edit op commits through the one
**synchronous `+0x290` `apply_sync`** path (see the 2026-07-12 writeup below) — no call site can pick the
crashing deferred path. `mkcmd` (`kind=1` prefab paste, a different operation that never crashed) stays on
the deferred staging path.

**A first port (commit `e7ee129`, 2026-07-09) was reset out** — it carried two bug patterns fixed since:
(1) every apply-op scheduled via the
**deferred `+0xd0`** path — the same split-commit-across-threads pattern that double-frees the decl-source
block on map teardown (see the 2026-07-12 deferred-apply writeup below); (2) several persistent `static`
scratch buffers (256 KB × multiple call sites, a 1 MB idstr table) — the same BSS-footprint pattern that
caused the controller-freelook regression on the `+0x298` slot. The shipped port fixes both: synchronous
`kind=0` commits, and all scratch
buffers heap-allocated transiently per call (`malloc`/`free`), never `static`/BSS.

**Store slots (`+0x2A0` `push_to_stack` / `+0x2A8` `clear_stack`).** Two vtable ext slots (ext 7/8) let the
frontend reach the backend-owned stores from the UI: `push_to_stack` wires the Entities-tab "Push to
stack 0" context action to the real stores (dedup on push; multi-select pushes the whole selection), and
`clear_stack` empties stack `index` from "Clear stack 0" instead of needing the DOOM console (`sh cstk 0`)
— it mirrors `h_cstk`'s own logic and returns the pre-clear count so the caller can toast a confirmation.
(An earlier reverted attempt used `+0x290` for push; that offset is now `apply_sync`, so these are fresh
appends — the append-only vtable discipline means no existing slot offset ever moves.)

**The bug that broke every decl-edit driven from the UI — `json_patch` mishandled a non-object `edit`.**
Live-tested `sh bss`/`sh acctargets` both returned `applied 0/1` with no decl change. The engine's own
deserialize `Lexer` rejected the patched text. Root cause (found by dumping the exact bytes fed to the
lexer): an entity you haven't hand-edited serializes its `state.edit` **overrides** block as `null` (or `{}`)
via the `+0xc8` serialize — everything the editor *shows* on it is **inherited** from its decl, not an
explicit override. When `json_patch` had to create a path segment (`targets`, `renderModelInfo.model`, …)
under a non-object `edit`, it spliced a bare `"key":value` **member** where a braced **object value** was
required, producing invalid `"edit":"targets":{…}` — the exact JS-`typeof null` / null-safety class of
gap, one layer
down in C. Fixed in `json_patch.c`: (a) the missing-path branches in `json_walk_set` / `json_walk_upsert_
reflist` now wrap the built chain in braces (`{…}`) so it's a valid object value for both the splice-into-a-
non-object and insert-new-member cases; (b) `json_insert_member`'s empty-object test corrected from
`p == obj_close` (always false — `obj_close` points *past* the `}`) to `*p == '}'`, which had been appending
a trailing comma (`{"k":v,}`) on an empty `{}` edit. Verified offline against a captured real entity plus
null/empty/missing-`edit` variants, then live: `bss`/`acctargets` apply `1/1`.

**New backend-exclusive SnapStack+ commands** (`chkstk` / `chkgrp` / `clrgrp`) + toast polish. Store
inspection/management the OG set lacked: `chkstk [N]` lists a stack (or summarizes all), `chkgrp [name]`
lists a group (or all groups), `clrgrp <name>|*` deletes a group. Also added confirm toasts to previously
silent ops (`cstk`, `phov`, `pop2g`, `pg` receiver/stack naming, `accl`/`acctargets` receiver toast) since a
stack/group op is otherwise invisible without a `chk*`, and a `pg`/`pop2g` single letter-first arg now
implies stack 0 (`sh pg mygroup` == `sh pg 0 mygroup`), consistent with how the operand resolver treats a
letter-first arg as a group everywhere else.

**Verified in-game:** `psel`, `popsel`, `phov`, `cstk`, `pr`, `pg`, `pop2g`, `filtinh`,
`filtcls`, `bss`, `bsi`, `bsf`, `bsb`, `bse`, `bsin`, `bscls`, `bsincls`, `accl`, `acctargets`, plus the new
`chkstk`/`chkgrp`/`clrgrp`/`snapstack_diag`. **`mkcmd` is ported (a faithful C
port of the handler + the byte-exact prefab template) but not live-verified** — flagged as a TODO.

## 2026-07-13 — Palette-Timeline portable-inherit normalize in a backend slot (`+0x298`); the decl-source blob lags the raw inherit by one commit

**What & why.** A Timeline placed from the in-game SnapMap palette is spawned from a repurposed
`snapmaps/editor_only/placeholder_target` entityDef (the only way to make a Timeline selectable in the
palette — the clone can't fabricate one directly), so the fresh entity records *that* as its `inherit`. A
map saved with it only reloads where our override is installed — not portable. The normalize rewrites the
inherit to the portable `snapmaps/unknown`; it lives in the backend vtable slot **`+0x298`
`normalize_timeline_inherit`** (`snapmap_plus_iface.h`/`.c`, `apply_engine.c` `slot_normalize_timeline_inherit`,
exported via `sh_apply_engine_get_slots`), and the frontend calls it from
its Timeline rescan.

**The bug the port exposed — the inherit blob lags one commit.** The normalize kept committing but the
Inherit box (and the *saved map*) stayed on the placeholder. Root cause is an ordering property of the
shared commit body `ae_apply_one`: it runs `DeclSourceRebuild` (which re-emits the decl-source blob at
`defsub+0x38`, baking in the inherit header from the *current* `defsub+0x58`) **before** it assigns the new
inherit to `defsub+0x58`. So after one commit the raw idStr reads `snapmaps/unknown` but the blob still
reads the placeholder — and `get_inherit` (`+0x50`) reads that blob, as does the map serializer. The
normalize's re-fire gate therefore **must read the blob (`defsub+0x38`), not the raw idStr (`defsub+0x58`)**:
gating on the raw field commits exactly once and leaves the blob stuck forever; gating on the blob keeps it
firing across rescans until a later commit's `DeclSourceRebuild` finally bakes `snapmaps/unknown` into the
blob. The
"repeated commits" in the log are load-bearing, not waste. (A raw-field gate was tried first and confirmed
in-game to leave the entity stuck on the placeholder; the DECL_BLOB_OFF gate fixed it.)

**Footprint note.** The slot heap-allocates its two 256 KB scratch buffers per call and frees them;
an earlier version used two persistent 1 MB static/BSS buffers and caused a
**controller-freelook regression** in-game (2 MB of resident BSS) — reverted, and
the reason it's heap-transient now.

> The "freshly-placed Timeline needs a save+reload" belief was never an engine limitation — it was the
> deferral crash plus a frontend JS bug (see below /
> `webview-ui.md`). Both fixed; fresh placement *and* reclass now save immediately.

**Architectural follow-up (since done):** at the time each decl-edit call site independently chose inline
(`+0x290`) vs deferred (`+0xd0`) — a footgun that bit twice this cycle (Save Timeline silently regressed
to the deferred crash path when one line was reverted). The durable fix — SnapStack as shared backend
handlers sending everything through one commit path — landed 2026-07-13/14; see the SnapStack entry above.

## 2026-07-12 — Confirmed: the decl/classname/inherit/displayname Save setters cannot overflow (contributor follow-up)

A contributor asked us to confirm `set_classname` (+0x78), `set_inherit` (+0x80), `set_displayname`
(+0x128), and `rebuild_set_declsource` (+0x40) have headroom for arbitrarily long input — the same
question that, for `PREFAB_TEMP_SIZE`, turned out to be a real stack-buffer overflow (see the
2026-07-06 entry below). Worth checking rather than assuming.

Live-RE against OG SnapHak (Beta 2, `snaphakui.dll`): the **Save to Decl** button's own click handler
(`FUN_1800175a0` → `FUN_180017d00`) reads each Qt field (decl text, inherit, classname, displayname),
computes its exact length, and passes that length explicitly into `FUN_180004a3c` for every one of the
four fields. Decompiling `FUN_180004a3c` shows a standard **growable string assign** (SSO threshold at
15 bytes, ~1.5x geometric growth, allocate-copy-free-old on overflow of current capacity) — the same
shape as `std::string::assign`, not a fixed-size buffer. There is no length cap anywhere in this path;
OG's own Save handler relies entirely on the assign function growing to fit, for all four fields.

This is architecturally unrelated to the `PREFAB_TEMP_SIZE` bug, which was a fixed-size **stack struct**
overflowed by a ctor call sequence — a completely different code shape from a dynamic string assign.
Our clone's `set_classname`/`set_inherit`/`set_displayname`/`rebuild_set_declsource` call the engine's
real assign functions directly (the same functions OG's own Save path calls), so they inherit the same
safety. **No code change needed** — confirmed safe by decompile, not by assumption.

## 2026-07-12 — SnapStack decl-edits double-freed the committed decl-source block; fixed with a synchronous inline apply (`+0x290`)

The long-standing SnapStack crash — run `acctargets` (or `bss`/`bsi`/`bsf`/`bsb`/`bse`), hit **Play**,
then trigger **any** map teardown (reload the same map, **New Map**, or load an unrelated map) and DOOM
dies with `"Memory corruption before block!"` — an access violation at
`DOOM+0x1ab32ee ← 0x19fd162` (`IdStrDtor`) `← 0x17ad00a` (decl-source teardown), `load_state=3`. No save
required; the corruption is in live memory, not the map file. The investigation ruled out the commit
content, the commit function, and a JSON-vs-node-tree architecture gap — each measured live against the
original and found byte-identical; this entry documents the backend mechanism and the fix.

**Root cause — the deferral, not the commit.** Live reverse-engineering of OG SnapHak (Beta 2) proved our
commit body `ae_apply_one` is byte-for-byte OG's `+0xd0` commit (`FUN_180004b80`), and our committed
decl-source blob is byte-identical to OG's. The *only* difference was **when** it runs. OG's SnapStack
handlers commit **inline, synchronously, on the UI/think-loop thread** (the `+0x1a0` work-queue drain).
Our clone ran the command handler on that same UI thread but **deferred** the heavy commit to the DOOM
main thread via `clone_bss_apply` at `ExecuteCommandBuffer` (the old "FIX B", added on the belief that the
structured deserialize AVs off the main thread). That split one atomic operation across two threads and
two frames, and left the freshly-committed decl-source block **double-owned** — so the next map teardown
freed it twice → heap-header corruption → the fault above.

The deferral's premise was stale: our `+0xc8` serialize already runs successfully on the UI thread, and
the reflection context it needs is a process-global singleton (engine `0x17f7030` → vtable `+0x80`),
reachable from any thread — OG proves it by committing on exactly that thread.

**Fix — a synchronous inline apply slot, `+0x290` (`apply_sync`).** Added to the matched-pair vtable ABI
(`snapmap_plus_iface.h`/`.c`, `apply_engine.c` `slot_apply_sync`, exported via `sh_apply_engine_get_slots` and
folded in by `iface_engine.c`). It runs the same per-item batch as the `clone_bss_apply` drain
(kind 0 = decl edit / 1 = mkcmd / 3 = target-write) but **inline on the calling UI thread**, so serialize +
commit are atomic and the committed block has a single clean owner — OG's exact flow. Because the commit
is now inline, callers pass their own item text with no deep-copy/pending store. Each `ae_apply_one` stays
SEH-guarded, so an off-main reflect gap (if it ever occurred) degrades to `applied 0`, never a crash. The
frontend routes all decl-edit ops through it; the deferred `+0xd0`
schedule is kept only as a fallback for an old backend without `+0x290`, and for `mkcmd`/prefab-paste
(`kind=1`), which never crashed and is left deferred. (During the hunt a `C2 SYNC apply: …` backend-log
marker confirmed the inline path ran; removed in the 2026-07-13 diagnostics cleanup.)

This supersedes the earlier "JSON round-trip vs in-memory node-tree edit" theory: our round-trip matched
OG's, so it was never the problem.

**Also migrated to `+0x290` the same day:** **Timeline Save** and the palette-timeline **inherit-normalize**.
The inherit-normalize is a one-shot fired by the UI's rescan
poll; on the deferred path it never persisted reliably, so the poll kept re-firing it (a `tl-inherit-portable`
toast on every logic-entity selection) — synchronous commit makes it a true one-shot. Fixing Timeline Save's
double-free also cleared the downstream timeline symptoms (copy/paste being wiped, needing to "save-backout"
before a timeline save).

> **Convention going forward — commit decl edits SYNCHRONOUSLY.** Any new operation that edits an entity's
> decl (serialize → patch → `ae_apply_one`) MUST commit inline via the `+0x290` `apply_sync` slot,
> NOT the deferred `+0xd0` `clone_bss_apply` schedule. The command
> handlers already run on the UI/think-loop thread where reflect resolves; deferring to the main-thread
> command buffer splits the operation across frames and double-owns the decl-source block. The deferred
> `+0xd0` path is retained only as an old-backend fallback and for prefab/mkcmd staging (`kind=1`), which
> stages into the paste slot rather than rewriting a decl.

**`ae_schedule_target_write` (`kind=3`) migrated to inline too, though it's DORMANT.** It writes
`state.edit.targets` onto the source entity's decl — but it **never fires in normal use**: `sh_target_any`
targets via SnapMap's native input/output-node logic and writes nothing to the decl (only `acctargets` ever
produces a `targets` list). So it was never a live crash risk. It's the natural primitive for a *future*
UI-driven "add target" feature, so it was migrated the same day (`ae_schedule_target_write` now calls
`ae_apply_target_write` inline, SEH-guarded, no deferral) as zero-risk future-proofing — that future feature
is crash-correct by default. Prefab Load/Place + `mkcmd` (`kind=1`) stage into the paste slot (different
mechanism, never crashed, intentionally left on the deferred path). *(Update 2026-07-13: WebView's Save
Timeline was migrated onto `+0x290` too — see the 2026-07-13 entry above; it was on the deferred `+0xd0`
path when this entry was written.)*

**DONE (2026-07-13):** the `AE_APPLY_DIAG` / `AE_DESER_DIAG` flags are now `0`, and the `+0x40 rebuild` and
`C2 SYNC apply` markers plus the `normalize-timeline-inherit … committed` log were removed — the backend no
longer emits the hunt-time diagnostics.

## 2026-07-08 — `apply_engine.c`: `ae_apply_one` could commit an empty class/inherit

Found while root-causing a hard crash-to-desktop / hang on returning to the SnapMap editor after
editing a Timeline, saving, and reloading the map (full investigation, including why the crash
turned out **not** to be caused by this bug, in [`webview-ui.md`](webview-ui.md)).

`ae_apply_one` (the shared `kind=0` commit body behind Save-to-Decl, Save Timeline, and
`wire-target`) deserializes the caller's full patched entity JSON onto a temp def, then copies that
temp def's normalized class/inherit/source back onto the live entity. The class/inherit copy only
null-checked the pointer (`if (clsPtr) ...`), not the string it pointed to — if the engine's own
`StructDeserialize` ever populated the temp def with a **non-null pointer to an empty string** (a
real, observed case, though not the one that turned out to matter for the Timeline crash — see
`webview-ui.md`), the guard let it through and committed `""` onto the live entity's classname or
inherit. A blank class/inherit is never valid; the next full map load fails with the engine's own
`"No class specified"` / `"Couldn't find map entity in entity palette '' inherit = "` and the entity
is unrecoverable.

**Fix:** the guard now also checks the first byte of the string
(`if (clsPtr && *(const char *)clsPtr) ...`, same for inherit) — an empty result is treated the same
as a null one and simply skipped (keep the live value), rather than committed. This degrades a choked
deserialize into "the edit didn't apply, entity intact" instead of "entity destroyed." Universal
across all three `kind=0` callers, not Timeline-specific — a blank class/inherit is never the right
outcome for any of them. Also confirmed to correctly *preserve* a non-standard inherit (e.g.
`snapmaps/editor_only/placeholder_target`) rather than requiring or defaulting to anything, unlike
the original's own Timeline-commit path, which hardcodes `inherit = "snapmaps/unknown"` on every
save (see `fidelity.md`) — the clone's keep-live behavior is strictly safer here.

## 2026-07-06 — `apply_engine.c`: `APPLY_TEXT_CAP` silently truncated large prefabs on Load/Place

Once Load/Place (staging via `kind=1`/mkcmd) was wired up and exercised against real prefab files,
some staged cleanly and some silently failed (backend log: `applied 0/1`, no crash, no fault-shield
entry -- nothing visibly wrong, the prefab just never showed up in the paste slot).

Root cause: `APPLY_TEXT_CAP` (a sanity ceiling on the JSON text carried in a scheduled apply item) was
`256 * 1024` -- sized for small per-entity edits, the only thing this pipeline used to carry. Real
prefab files can run well past that. `slot_schedule_apply`'s batch deep-copy silently truncated
anything over the cap with no error at all, so an oversized prefab got cut off mid-JSON before it ever
reached the deserializer, which then failed to lex the truncated text.

Diagnosed with a step-by-step trace added to `ae_deserialize_to_obj` (gated behind `AE_DESER_DIAG_ON`,
mirroring the existing `AE_SER_DIAG` pattern on the serialize side) -- it pinpointed the exact failing
prefab's text arriving already truncated to one byte under the old cap, confirming the truncation
happened upstream in scheduling, not in the deserialize call itself.

**Fix:** raised `APPLY_TEXT_CAP` to 4 MB, matching the scratch buffer already used elsewhere for
prefab content. Retested against every prefab that had been failing (all `applied 1/1` now) and
live-tested up to 2 MB with no issue -- comfortable headroom over anything hit so far. This cap is also
used as a sanity bound when reading a serialized result back out (`ae_read_idstr`), so raising it
benefits the Create-from-selection direction too, not just Load/Place.

## 2026-07-06 — `apply_engine.c`: prefab create-from-selection crashes

Two independent bugs, both in the `+0xb0` serialize-selection path (`slot_serialize_selection`),
found back to back while root-causing a hard DOOM crash on every "Create from selection" call. This
backend path had never been exercised before the UI's first real call
into it.

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

## 2026-07-06 — `apply_engine.c`: Load/Place, calling `PasteInstantiate` directly was wrong

First implementation of `kind=2` (Load/Place) staged the prefab (`ae_mkcmd_one`, same as `kind=1`)
then called `PasteInstantiate` (`+0x54f950`) directly, mirroring how `PrefabPopulate`/`PrefabCtor`
are called elsewhere. This looked reasonable but was wrong in a way the crash didn't immediately
reveal: the placed entity came out selected but not draggable, and returning to the editor from Play
afterward crashed DOOM hard (`ACCESS_VIOLATION` reading `0x0` at `+0x5a516b`, a large call stack
through several engine frames).

Root cause: the engine's real native Ctrl+V handler (`+0xce1810` in `DOOMx64vk.exe`) calls
`PasteInstantiate`, then calls a *second* function (`+0xcf35e0`) that sets grab-tool state (bit
manipulation at `obj+0x2d0`/`+0x2d1`, `obj+0x1ac = 4`, `obj+0xbb8 = 1`) on some object passed into the
handler as its 2nd argument. Calling `PasteInstantiate` alone skips that second call entirely, leaving
the engine in whatever inconsistent state that omission causes — undraggable placement now, a crash
on world transition later.

Identifying *which* object needs those writes turned out to be a dead end via live debugging:
interactive breakpoint-based verification (attach, set a breakpoint, `debugger_continue`, wait for a
user action to hit it) is **confirmed broken** in this environment — it reliably freezes DOOM's input
even though the debugger reports the process as running, regardless of what triggers the breakpoint
(tried 4 times, different breakpoints, different triggers). A static hypothesis (the object is the
same selection object behind `hovered_id`) didn't hold up against a read-only memory check either —
the candidate field region held array-like data, not simple flags.

**Fix — sidestep the problem entirely:** confirmed against the *original* `snaphakui.dll` +
`XINPUT1_3.dll` (static Ghidra decompilation, no live debugging needed) that neither ever calls
`PasteInstantiate` directly either. A prefab double-click in the original UI
(`snaphakui.dll!FUN_180017538` → `FUN_180013878`) only reads the file and stores its text; the actual
"deserialize into `editor+0x209a8`" step is a separate function (`XINPUT1_3.dll!FUN_180006bf0` →
`FUN_1800094f0`) — the same operation as our own `ae_mkcmd_one`. Placement always happens through a
real, native Ctrl+V; nothing in either original DLL calls `PasteInstantiate` at all. So `kind=2` no
longer calls it either — it just stages (`ae_mkcmd_one`, identical to `kind=1`), and something else
finds the game's own top-level window (`EnumWindows`, same-process, skipping our own
`"SnapHakStudioWebView"` companion window), brings it to the foreground, and synthesizes a real Ctrl+V
via `SendInput`, letting the engine's own already-correct native handler do the actual instantiate +
grab-tool setup, exactly as it would for a real user paste — no need to ever identify the mystery
object at all.

**First attempt at the synthetic-input step was on the wrong thread.** Put the `SetForegroundWindow` +
`SendInput` call directly inside `ae_mkcmd_and_place`, which runs on DOOM's own main thread (inside the
engine's `clone_bss_apply` command-buffer drain). Staging succeeded (confirmed via the toast, "applied
1/1") but no paste ever happened, even though the user could then paste manually with Ctrl+V — proving
the staged data was valid the whole time. `SetForegroundWindow` requests a focus change but doesn't
apply it synchronously; the window that needs it processes the change on its own message pump. Calling
it from DOOM's main thread and then immediately (same thread, no yield) calling `SendInput` fires
before that thread ever gets to pump the message that would complete the focus switch — so the
keystroke landed nowhere. **Fix attempt 2:** moved the whole window-find + focus + `SendInput`
sequence out of `apply_engine.c` entirely and into the webview UI's own think-loop
(`poc_synthesize_native_paste` in `snapmap_plus_ui_webview.cpp`), which has its own message pump and isn't
blocking DOOM's simulation. Also deliberately delayed ~6 loop iterations (~200ms) after a successful
schedule, plus a 50ms sleep between the focus request and the keystroke, so the engine's command-buffer
drain has actually staged the prefab, and the focus change has actually landed, before Ctrl+V fires.

**That fix moved the keystroke correctly but surfaced a third side effect.** Staging still succeeded,
and this time the focus switch itself visibly worked -- but forcing `SetForegroundWindow` on DOOM's
window made the game pop its own ESC/pause menu, almost certainly a "regained focus after being
alt-tabbed away" safety behavior (our webview panel is a separate top-level window, so from the game's
perspective, forcing focus back to it looks exactly like the player alt-tabbing back in). The
synthesized Ctrl+V then likely lands on that menu instead of the 3D view, so nothing pastes -- but
again, the staged data is untouched and a manual Ctrl+V still works once the menu is dismissed.

**Decision: stop automating this step.** Three attempts, three different side effects (a hard crash, a
silent no-op, an unwanted pause menu), and each only surfaced by actually testing in DOOM. Given how
consistently fragile this specific corner of the engine has been, `kind=2` was removed entirely --
Load/Place is back to plain `kind=1` (identical to the `sh mkcmd` staging path): stage the prefab, then tell the
user to press Ctrl+V themselves. This matches the *original* SnapHak's own actual workflow exactly (see
the double-click investigation above) and needs zero window-focus tricks, zero risk of the side effects
above, and zero remaining need to ever identify the mystery grab-tool object.
