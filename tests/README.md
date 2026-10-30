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
  RIP-relative LEA, RIP-relative 64-bit load and relative-call relocation with
  positive and negative displacements, preserved call arguments and return flow,
  and the refused load destinations.
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
- `rawmap_paths_test.c` also drives the load and save detours: a local save, a
  published map and a staged rawmap that carry an unconvertible `smpkg.`
  package load as plain maps (the variables removed, `allocCount` corrected, one
  verdict line), lookalike names stay, and a save keeps the engine's own bytes.
- `modmanifest_test.c` checks the `smpmod1.` manifest rows: grammar, the value
  round trip, rows appended last and stripped with `allocCount` restored, the
  per-row budget that keeps them out of the 4 KiB playtest message, placed names,
  the save closure and the load step against serving-layer doubles.
- `modpublished_test.c` checks the published-map and offline-play gates with
  the real manifest and engine doubles: return-address classification, vanilla
  and installed maps passing through, the hold and the question on a later
  tick, Yes fetching on a worker thread then activating and resuming with the
  retained text, No, a failed fetch, a bundle that will not install, another
  load or a newer launch abandoning the hold mid-fetch, the joiner's host
  re-check, the warm-cache joiner branch and My Maps play.
- `modlegacy_test.c` checks the legacy carrier policy: header grammar per era,
  completeness and digest, stored and deflate ZIP members, the older layout
  mapping, conversion, cleaning and verdict lines with a builder double, and
  the serving-index rescan a new conversion requests. Set
  `SH_MODLEGACY_FIXTURES` to a folder holding the campaign's carrier maps to add
  an integration pass; it is skipped otherwise. The maps are not in the
  repository because their packages contain game files.
- `modnet_test.c`, `modsync_test.c` and `modexport_test.c` cover the store client, the upload,
  download, release and retry flows, and the rawmap ZIP export and import against a loopback stub
  of the store (`mod_store_stub.h`); no real network is used.
- `modpublish_test.c` checks the publish side against the loopback stub with
  engine doubles: a save without rows passing through, the over-bound refusal,
  the first-time question (No cancels and leaves the setting, Yes sets it,
  uploads and resumes, the toast route agrees on the next publish), a failed
  upload cancelling, the editor leaving and another save, the claim at the
  feed post with and without a stored secret, a republish replacing the set,
  release on a republish without mods and on unpublish, the copy gate on both
  sides of the bound, and the launch step settling provisional refs and
  retrying the queue.
- `mod_seam_stub.c` stands in for the serving layer and the builder entry in
  tests that link the map manifest or the legacy converter without them.
- `modbundle_test.c` builds `fixtures\mods\alex.r2-imp` and pins its bundle
  hash against `alex.r2-imp.canonical.json`; it also checks logical names, the
  needs scan, build numbering and each refusal rule. The fixture files are
  hashed byte for byte, so their line endings are never converted.
- `modstore_test.c` installs the fixture into a temporary store: blobs, hard
  links and the copy fallback, verification and corruption, in-place builds and
  need resolution across installed bundles.
- `modserve_test.c` checks the serving index: admission and failure isolation,
  lookups in every spelling, provider precedence against the real `overrides.c`,
  probe claims, registration plans and their order, the rescan and the lookup
  cost.
- `mod_pipeline_test.c` links the builder, the serving index, the manifest and
  the legacy converter together: a built fixture is admitted, saved as one
  `smpmod1.` row and activated on the next load; a legacy-form bundle with
  non-stock shadows builds and registers only its new names; skins decls pass
  by their mesh path. With `SH_MODLEGACY_FIXTURES` set, each carrier map is
  converted with the real builder and activated in the same parse.
- `modui_test.c` runs the Mods tab's backend against a temporary store, the
  real serving index and the map manifest: the document on an empty store,
  install through the tab, every state string, manifest rows and the placed
  closure, stock shadows and a conflict, the removal of a covered build, the
  main-thread refusal and the slot's size query. It also checks that every key
  in `fixtures\mods_tab_payload.json` is one the backend writes.
- `mods_tab_test.js` renders the Mods tab from that fixture with a minimal DOM:
  the windowed lists, the lazily built shadow list, the action messages, the
  empty and error states, and the page, host and interface contract.
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
