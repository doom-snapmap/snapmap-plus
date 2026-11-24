# Release and derivation tools

## Purpose

Contains maintainer tools for release notes, the package check, the generated tables, frontend assets and portable engine signatures.

## Contents

- `changelog.py` parses and validates `CHANGELOG.md` and extracts a release's notes.
- `draft_changelog.py` prepares release-note drafts for review.
- `sync_release_notes.py` synchronizes reviewed notes to GitHub releases.
- `cbuild.ps1` holds the shared MSVC build helpers the component build scripts and `tests/run-tests.ps1` dot-source: one vcvars import, incremental parallel compiles, the link step and a bounded process pool.
- `bundle_guard.ps1` checks a packaged `dist/` folder before it is published (CI and `release.yml` run it).
- `gen_programs.js` embeds the render programs' shaders (`src/backend/programs/shaders/*.rp`) in the backend; `gen_script_sdk.js` embeds the script SDK (`src/sdk/lua/*.lua`) as `src/backend/script/script_sdk_lua.c`. Each takes `--check`.
- [worker-tests/](worker-tests/) pins miniflare (`package.json`, `package-lock.json`) for the Worker runtime suite.
- `update-avatar.ps1` deliberately refreshes the embedded organization avatar; normal builds use committed assets.
- [signatures/](signatures/README.md) derives and checks engine patterns.
- [tests/](tests/) tests changelog parsing and drafting.
- `requirements.in` and `requirements.txt` declare and pin release-drafter dependencies.

## Working here

From the root, run `python tools/changelog.py --lint` and `python -m unittest discover -s tools/tests -t tools -v`. Those checks do not call a paid service. Drafting and release synchronization are maintainer operations; follow [contributing](../docs/contributing.md) and [services](../docs/services.md).

For local prompt testing, use `python tools/draft_changelog.py --dry-run --tag <next-tag> --base <previous-tag>`.
With the pinned dependencies installed and `ANTHROPIC_API_KEY` supplied locally, this prints the draft
and its sources without contacting GitHub or writing output files. It uses the paid drafting API;
see the contributing guide for setup and input scope.
