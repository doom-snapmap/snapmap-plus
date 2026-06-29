# Architecture

A contributor-orientation map of how the two DLLs fit together. For the *what it does*
feature list see [`capabilities.md`](capabilities.md); for the deliberately-faithful quirks see
[`fidelity.md`](fidelity.md).

## The two DLLs and the boundary between them

The clone is a **backend** (`XINPUT1_3.dll`, built from `src/backend/`) and a **frontend**
(`snaphakui.dll`, built from `src/ui/`).

- The backend loads first. DOOM loads `XINPUT1_3.dll` at startup (it sits in the game root and
  forwards the real XInput exports through to System32). Once running, the **backend** does
  `LoadLibraryA(".\\snaphak\\snaphakui.dll")` and then `CreateThread(snaphak_ui_init, ...)` to
  bring the Qt window up on its own thread.
- The frontend never touches the engine directly. Every engine read or write the UI needs goes
  through a shared **interface object** that the backend creates and hands to the frontend's
  init thread.

This split is the version-portability story: all the build-specific engine offsets and
signature-resolved engine calls live **behind the interface, in the backend**. The frontend
holds no raw engine addresses, so a DOOM update only forces a re-derive on the backend side.

## The 3-object model

The frontend is organized around three objects (`snaphak_ui_init`, RVA 0x129d0 in OG):

| Object | Type | Role |
|---|---|---|
| **window** | `QMainWindow` | The visible widget tree — the 6 tabs, the Camera-Origin groupbox, menu/tool/status bars. Built by a hand-written `setupUi` that mirrors the OG widget layout exactly. |
| **WIN** | controller | The frontend's own state: cached child-widget pointers, the `WIN[0]` flag word that drives the per-frame dispatch, and `WIN[4]` = the interface pointer. Not a Qt object. |
| **interface** | backend-owned bridge | The cross-DLL ABI object (see below). The backend creates and owns it; the frontend only calls through its vtable. |

`snaphak_ui_init` constructs the `QApplication`, builds the window, runs `setupUi`, stores the
interface on `WIN[4]`, `show()`s the window, then enters the think-loop and never returns.

## The 30 Hz manual think-loop (not `QApplication::exec()`)

The frontend does **not** use Qt's event loop. It runs its own pump (OG `FUN_180015c04`), once
per frame at roughly 30 Hz, under a loop mutex:

```
lock(loop_mutex)
    drain the deferred-record tree
    (*(interface + 0x1a0))()      // drain the backend work-queue: run queued {handler, args}
    poll EntityMode
    if (interface ready-poll)  ->  flag-word dispatch (consume WIN[0] bits)
unlock
processEvents(0)                  // pump Qt events manually
Sleep(33ms)                       // ~30 Hz
```

This is **load-bearing**, not a stylistic choice. Heavy engine work (the SnapStack apply chain,
Save-to-Decl, timeline commits) must run on the main thread; the manual pump plus the `+0x1a0`
work-queue drain *are* the main-thread execution point. A UI-thread or RPC-thread engine call
deadlocks the engine's command-system lock, so work is enqueued and drained here instead.
Replicate the pump; do not swap in `exec()`.

## The 77-slot interface vtable (the matched-pair ABI)

The shared interface object is defined once, in `src/common/snaphak_iface.h`, and **both DLLs
include that header** — it is a matched pair. The backend writes the vtable and fields; the
frontend reads them at the same offsets.

- The backend builds it (`operator_new(0x60)`), installs a 77-slot vtable (`+0x00..+0x260`),
  initializes the mutex at `+0x08`, and hangs a sub-object off `+0x58` that holds the SnapStack
  subcommand map and the main-thread work-queue.
- The frontend calls vtable slots for everything it needs from the engine: entity
  count/validity, classname/inherit/displayname read and write, serialize/deserialize an
  entity, apply an edit (`+0xd0`), enqueue and drain the work-queue (`+0x90` / `+0x1a0`),
  register/unregister SnapStack subcommands (`+0x188` / `+0x190`), enumerate decls, manage the
  selection, and show toasts (`+0x1b8`).

Because this vtable is the *clone's own* ABI — not a DOOM structure — it is self-consistent and
not DOOM-build-dependent. The only hardcoded offsets that cross the DLL line are these vtable
slot offsets and the `WIN[...]` field offsets. **They must stay pinned identically in both
DLLs**; the two are a matched set. The build-specific *engine* offsets sit behind the vtable in
the backend, where they are re-derived per build.
