# Signature and global derivation tooling

These scripts produce the two things that let one build of Snapmap+ serve more than one
DOOM 2016 executable:

- the masked byte signatures in `src/backend/signatures.c`, and
- the generated table in `src/backend/engine_globals_table.gen.h`.

Nothing here ships in the DLL. You need it only when adding a new engine function or data
global, or when DOOM is patched and addresses move.

## Why this exists

DOOM 2016 ships two executables built from one source tree and linked one second apart:
`DOOMx64vk.exe` (Vulkan) and `DOOMx64.exe` (OpenGL). Their import tables are identical apart
from `vulkan-1.dll` versus `OPENGL32.dll`, and the game relaunches itself into the other one
when the `r_renderAPI` cvar changes — so a user can end up in either from a single Steam launch.

Between those two images, function addresses shift by −0x400 to −0xE460 with no uniform offset,
and data globals move by nearly 0x1000000. Struct field layouts, on the other hand, are
identical. So the rule the product follows is:

> **Identity comes from a signature. Addresses come from derivation. Never from a constant.**

A hardcoded RVA is a fact about one link output. It is not wrong in a way you can detect at
runtime — it silently names unrelated memory — which is why the product refuses rather than
falls back to one whenever it is not running the build that RVA came from.

## Getting unpacked images

Both executables are SteamStub-wrapped, so their `.text` is encrypted on disk. Unpack each with
[Steamless](https://github.com/atom0s/Steamless) before running anything here:

    Steamless.CLI.exe "…\DOOM\DOOMx64vk.exe"
    Steamless.CLI.exe "…\DOOM\DOOMx64.exe"

That writes `<name>.unpacked.exe` alongside each input.

Requires Python 3.10+ with `capstone` and `numpy`.

## The scripts

`extract_sig.py <ref.exe> <target.exe> <rva> [...]`
Extract a minimal unique signature for a function, growing the window one instruction at a time
until it matches exactly once on the reference image, then require it to match exactly once on
the target image too. Wildcards RIP-relative displacements and branch rel32s; keeps struct
displacements fixed, because those are identical across builds and raise selectivity for free.
This is what you use when adding a normal entry to `BACKEND_ENGINE_SIGNATURES`.

`derive_callee.py <ref.exe> <target.exe> <rva> [...]`
For functions that cannot be identified by their own bytes — a prologue shared with dozens of
other functions, a stub too short to anchor, a body that is a generic shape. Signs a *call site*
instead and reads the `call rel32`. Reports multi-site consensus.

`derive_global.py <ref.exe> <target.exe> <rva> [...]`
For data globals. Finds every RIP-relative code site that computes the address, builds a masked
signature of that site, requires uniqueness on both images, then reads the displacement at the
match on the target. Reports multi-site consensus.

`gen_globals_table.py [derived_globals.json] [out.h]`
Emits `engine_globals_table.gen.h` from the derivation output. **The table is generated. Never
hand-edit a pattern in it.** Adding a global means adding a name to the `NAMES` map here and
re-running, not writing C.

`sigscan.py <signatures.c> <image> [<image> ...]`
A quick host-side check of the whole shipped database against one or more images, without
compiling anything. `tests/run-tests.ps1 -Doom … -DoomAlt …` is the authoritative gate; this is
for iterating.

## Adding a signature

1. Find the function's RVA on an unpacked image.
2. `extract_sig.py <vk> <gl> <rva>`. If it reports `PORTABLE`, take the pattern.
3. If it reports `FAILED`, the function has no identity of its own — use `derive_callee.py` and
   add it to the globals table instead.
4. Add the entry, then prove it:

       tests\run-tests.ps1 -Doom …\DOOMx64vk.exe.unpacked.exe -DoomAlt …\DOOMx64.exe.unpacked.exe

   Both images must resolve it uniquely. A pattern that is unique on only one image is a
   coincidence, not an identity, and shipping it means the product works on one build and
   mis-resolves on the other with no symptom you would notice.

`RenderLogStub` in `signatures.c` is the cautionary example already in the tree: its pattern is
`mov [rsp+0x20],r9; ret` plus padding, which is a *shape*. It happens to land on the right
function on both images, confirmed separately by five agreeing call sites — but nothing about the
pattern earns that, and only the two-image gate would have caught it if it had not.

## Supporting another DOOM build

Unpack it, run the derivations against it, and check that every existing signature and global
still resolves. Nothing about the product needs to know which build it is running. If a pattern
stops resolving there, fix the pattern — do not add a per-build address table. There isn't one,
and adding one is how a single product turns into two.
