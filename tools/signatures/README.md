# Signature derivation

## Purpose

Derives and checks the byte patterns that locate engine functions and data
globals on the Vulkan and OpenGL executables of DOOM 2016 build 13954591 (the
April 2024 update, the only build Snapmap+ supports). These tools support
porting and maintenance; they are not included in the DLLs.

## Contents

| Script | Responsibility |
|---|---|
| `extract_sig.py` | Grow a masked function signature and check uniqueness on two images. |
| `derive_callee.py` | Identify a function through signed callers and decoded call targets. |
| `derive_global.py` | Find signed RIP-relative references to a data global. |
| `gen_globals_table.py` | Regenerate the engine-global table from the committed rows plus an update file (`globals-13954591.json`). |
| `globals-13954591.json` | Build 13954591 anchors and Vulkan/OpenGL RVAs for every global (from the rederivation record). |
| `sigscan.py` | Check every committed pattern and anchor against images; other builds are an optional cross-check. |

## Working here

Use Python 3.10 or later with `capstone` and `numpy`. The build 13954591
executables are not SteamStub-wrapped, so the shipped files are scanned directly.
Do not commit the images or extracted game bytes.

The output feeds `src/backend/signatures.c` and
`src/backend/engine_globals_table.gen.h`. Preserve the distinction between a
signature, which finds code, and an RVA, which describes one image. Renderer
addresses cannot be translated with a single offset.

## Commands

```text
python extract_sig.py <reference.exe> <target.exe> <rva> [...]
python derive_callee.py <reference.exe> <target.exe> <rva> [...]
python derive_global.py <reference.exe> <target.exe> <rva> [...]
python gen_globals_table.py [--header H] [--update globals-13954591.json] [--out OUT]
python sigscan.py <image> [<image> ...] [--json <out.json>]
```

Run from this directory or use each script's full path. Function extraction
masks RIP-relative displacements and relative branches while retaining structure
displacements. Callee and global derivation use multiple sites to check agreement
where possible. Read each script's argument handling before selecting output paths.

## Adding or repairing an entry

1. Identify the intended function or global on a known unpacked image.
2. Derive a signature and require a unique match on both renderer images.
   A shared prologue or tiny stub may need a caller anchor instead.
3. Record each RVA for its own renderer (`rva_vk`, `rva_gl`; the two executables
   are separate links). Do not treat a unique byte match alone as proof that it
   names the intended operation.
4. Update the source entry or generator input. Never hand-edit a generated
   pattern in `engine_globals_table.gen.h`.
5. From the repository root, run
   `tests/run-tests.ps1 -Doom <DOOMx64vk.exe> -DoomAlt <DOOMx64.exe>` with the
   build 13954591 executables.

The native two-image suite is the required gate; `sigscan.py` is an iteration
aid. Confirm calling conventions and field layouts separately when supporting
a different engine build. See [contributing](../../docs/contributing.md#7-run-the-tests).
