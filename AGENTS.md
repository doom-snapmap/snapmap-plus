# Agent Instructions

Before changing code or handling Git in this repository, read and follow
`docs/contributing.md`. Its build, test, documentation, commit-message, and
pull-request requirements are mandatory.

Before creating a commit or merging a pull request, reread "Writing the commit
body" in that document. A commit body is not published: it is read by the next
maintainer and used as input when drafting a release's notes, which live in
`CHANGELOG.md` and are reviewed before they ship. Keep it to one to three short
sentences (about 60 words, never more than 80) saying what changed and why.
Never concatenate commit bodies when squashing; write one fresh body for the
whole pull request. Longer rationale and evidence go in the pull-request
description or in `docs/`.

Release notes are prepared with `prepare-release.yml` and reviewed as a pull
request before the tag is pushed -- see "Cutting a release" in
`docs/contributing.md`. Never hand-edit a release body on GitHub; edit
`CHANGELOG.md`.
