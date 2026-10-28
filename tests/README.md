# Runtime regression tests

## Purpose

Checks native helpers, engine-interface contracts, frontend JavaScript and optional resolution against local DOOM executables.

## Contents

- `run-tests.ps1` builds and runs the native and JavaScript suites. `-Only`
  accepts complete native test names, including `_test`, and rejects every
  unknown selection instead of reporting success for an empty or partial run.
  `-Skip` omits explicitly named native suites, reports each omission, and still
  runs JavaScript when `-Only` is absent. Unknown names or conflicting selectors
  fail; the default continues to run every suite.
- `*_test.c` and `*_test.cpp` exercise runtime helpers and source contracts.
- `*_test.js` checks page behavior, browser modules and the feedback relay.
- `nav_menu_test.c` checks native action ordering, progress and completion,
  repeated requests, cancellation, and rollback after a partial installation.
  `nav_bake_test.c` collects background builds without preview drawing.
  `apply_dispatch_test.c` checks Start-menu scheduling and worker completion.
- Installer onboarding tests cover original SnapHak profile relocation, exact
  copies and idempotency, and check that retired package folders are neither
  copied nor created.
- `nav_heap_test.c` checks full path-search queues, repeated route improvements,
  allocation canaries and the subtree metadata consumed by native removal.
- `nav_heap_hook_test.c` checks queue hook publication, rollback and bounded
  insertion through the installed detour with an authored native-code fixture.
- `hook_test.c` executes hook publication and restoration, including explicit
  RIP-relative LEA and relative-call relocation with positive and negative
  displacements, preserved call arguments and return flow.
- `rawmap_paths_test.c` checks save destinations, live exports and read-only
  snapshots that leave no save side effects.
- `config_json_test.c` separates strict UTF-8 documents from native byte strings,
  preserving every high byte and mixed text while rejecting invalid syntax,
  escapes, duplicate keys, raw controls and excess nesting.
- `sig_test.c`, `globals_test.c` and `hooktol_test.c` use optional local game images.
- `startup_bindings_test.c` checks early observer binding, delayed code readiness,
  preserved pre-install addresses and partial startup without repeated hooks.
- `serialized_entities_guard_test.c` executes the serialized-list cleanup guard
  with valid and empty handles, checking CPU state and patch rollback ownership.
- `config_json_test.c` checks rejection reasons and byte offsets as well as
  strict validation.
- `map_payload_test.c` executes authored native save/read length-check fixtures
  before and after repair, including signed boundaries, signature refusal and
  rollback of both sites after a partial failure. Rawmap path tests also read
  a file over 64 MiB.
- `rawmap_paths_test.c` also drives the load and save detours without the
  retired package layer: a local save, a published map and a staged rawmap that
  carry `smpkg.` package variables load as plain maps (the variables removed,
  `allocCount` corrected, one log line), lookalike names stay, and a save keeps
  the engine's own bytes.
- `startup_dirs_test.c` runs the startup folder creation and settings
  initialization against a temporary root and checks that only the root,
  `strings\` and `prefabs\` appear, then scans the backend, frontend and
  installer sources for any remaining mention of the retired package folders.
- `strids_test.c` checks user-document precedence over the built-in strings,
  escapes, refusal of invalid documents, rearm after native sorting, retirement
  of removed keys, sort-hook recovery and native table limits.
- `decl_scroll_probe.html` is a manual declaration-editor probe.

## Working here

From the repository root, run `powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-tests.ps1` with Node 22.13 or newer installed. Build outputs go to ignored `tests/obj/`.

The Worker runtime suite runs the feedback relay under Miniflare against a GitHub mock. Install its pinned
dependency with `npm install --prefix build/worker-tests --no-audit --no-fund --no-package-lock miniflare@4.20260730.0`, then run
`node tests/worker_runtime_test.js build/worker-tests/node_modules/miniflare`.

Changes to signatures or engine-global anchors require `-Doom <unpacked-image> -DoomAlt <other-renderer-image>`. See [contributing](../docs/contributing.md#7-run-the-tests) for the full requirements. Installer and release-tool tests live with those components. Preserve fixture data unless intentionally changing the case it represents.
