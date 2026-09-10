# Runtime regression tests

## Purpose

Checks native helpers, engine-interface contracts, frontend JavaScript and optional resolution against local DOOM executables.

## Contents

- `run-tests.ps1` builds and runs the native and JavaScript suites.
- `*_test.c` and `*_test.cpp` exercise runtime helpers and source contracts.
- `*_test.js` checks page behavior, browser modules and Worker services.
- `sig_test.c`, `globals_test.c` and `hooktol_test.c` use optional local game images.
- `map_package_fixtures.h` contains packaged test vectors.
- `decl_scroll_probe.html` is a manual declaration-editor probe; `strip_dump.c` is a diagnostic helper.

## Working here

From the repository root, run `powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-tests.ps1` with Node 22.13 or newer installed. Build outputs go to ignored `tests/obj/`.

The Worker runtime suite uses local storage and a GitHub mock. Install its pinned
dependency with `npm install --prefix build/worker-tests --no-audit --no-fund --no-package-lock miniflare@4.20260730.0`, then run
`node tests/worker_runtime_test.js build/worker-tests/node_modules/miniflare`.

Changes to signatures or engine-global anchors require `-Doom <unpacked-image> -DoomAlt <other-renderer-image>`. See [contributing](../docs/contributing.md#7-run-the-tests) for the full requirements. Installer and release-tool tests live with those components. Preserve fixture data unless intentionally changing the case it represents.
