# Release and derivation tools

## Purpose

Contains maintainer tools for release notes and portable engine signatures.

## Contents

- `changelog.py` parses and validates `CHANGELOG.md` and emits website data.
- `draft_changelog.py` prepares release-note drafts for review.
- `sync_release_notes.py` synchronizes reviewed notes to GitHub releases.
- [signatures/](signatures/README.md) derives and checks engine patterns.
- [tests/](tests/) tests changelog parsing and drafting.
- `requirements.in` and `requirements.txt` declare and pin release-drafter dependencies.

## Working here

From the root, run `python tools/changelog.py --lint` and `python -m unittest discover -s tools/tests -t tools -v`. Those checks do not call a paid service. Drafting and release synchronization are maintainer operations; follow [contributing](../docs/contributing.md) and [services](../docs/services.md).
