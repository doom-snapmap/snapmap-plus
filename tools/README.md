# Release and derivation tools

## Purpose

Contains maintainer tools for release notes, frontend assets and portable engine signatures.

## Contents

- `changelog.py` parses and validates `CHANGELOG.md` and emits website data.
- `draft_changelog.py` prepares release-note drafts for review.
- `sync_release_notes.py` synchronizes reviewed notes to GitHub releases.
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
