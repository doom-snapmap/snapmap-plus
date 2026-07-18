# Feedback pipeline

How an in-app report becomes a tracked issue, end to end — and what keeps the tracker clean. The parts:
the **dialog** (in the Studio UI), the **relay** (a Cloudflare Worker, [`feedback/`](../feedback/README.md)),
and two **hygiene workflows** (GitHub Actions in this repo).

```
User clicks "?" -> Send-feedback dialog (category / title / details / optional contact)
  -> the UI host POSTs the report over HTTPS to the relay        (src/ui/webview/, one worker thread)
    -> the relay validates it and, holding the repo credential:
         same report already open?  -> appends a confirmation comment to that issue
         otherwise                  -> creates a new issue, labeled by category + release channel
  -> the user sees a green "Report sent" toast (or red on failure -- nothing typed is lost)
```

## Why a relay

GitHub has no anonymous write path — filing an issue requires a token, and a token must never ship
inside a public binary. The relay is the smallest possible fix: a stateless Worker that accepts the
app's unauthenticated POST and files the issue with a fine-grained PAT (this repo only, Issues
read/write only) stored as a Worker secret. Users need no account of any kind. Deploy + token
rotation runbook: [`feedback/README.md`](../feedback/README.md).

## What a filed issue looks like

- **Title:** `[Bug] <the user's title>` (or `[Feature]` / `[Docs]` / `[Other]`).
- **Body:** the user's details, then a metadata block: `- Version: 0.2.0-beta.3 (beta)`, an optional
  `- Contact:` line, and an invisible `<!-- report-sig:… -->` marker (the dedup signature: a hash of
  category + normalized title). Nothing else is collected — no system info, no telemetry.
- **Labels:** the category (`bug` / `enhancement` / `documentation` / `question`), the release channel
  (`beta` / `stable`, from the version string; dev builds get neither), and `user-report` (marks
  relay-filed issues so the hygiene workflows only ever touch those).

**Dedup:** a report whose signature matches an *open* issue becomes a comment on it ("Another report of
this, on version …") instead of a duplicate — one issue with N confirmations. Closed issues are never
resurrected (closed means resolved or rejected; a fresh report opens a fresh issue). Matching is exact
by design; judging that two differently-worded reports are the same bug stays a maintainer call.

## Tracker hygiene (the two workflows)

Both are secretless (the built-in `GITHUB_TOKEN`, least-privilege `permissions:`), SHA-pinned like every
workflow here, and scoped to `user-report` issues — they never touch maintainer-filed issues or PRs.

- [`issues-retest.yml`](../.github/workflows/issues-retest.yml) — on every published release, open
  user reports filed on an **older** version get a "still reproducible on vX?" comment + the
  `needs-retest` label. A pre-release only prompts `beta`-channel reports; a stable release prompts
  everything older. Deliberately not auto-close-on-release: an old-version bug usually still exists —
  real fixes are closed by a maintainer with "fixed in vX".
- [`issues-stale.yml`](../.github/workflows/issues-stale.yml) — nightly: issues labeled `needs-retest`
  or `awaiting-response` (i.e. waiting on their reporter) get a warning after 21 quiet days and close
  14 days later. Confirmed bugs and feature requests never auto-close.

Label glossary: `user-report` (relay-filed), `beta`/`stable` (reported channel), `needs-retest`
(superseded by a release, awaiting confirmation), `awaiting-response` (maintainer asked the reporter a
question — apply by hand to arm the stale sweep), `stale` (the sweep's warning marker).

## Failure modes

| Failure | What happens |
|---|---|
| Relay down / offline / DNS | Red toast in-app ("could not send"); the dialog stays open with everything typed. |
| PAT expired (annual rotation missed) | Same red toast; fixed by rotating the secret — [`feedback/README.md`](../feedback/README.md). |
| Spam | Honeypot + size caps in the relay; escalation is a Cloudflare rate-limit rule (dashboard, no code). Worst case is deletable spam issues — the token can't touch anything but Issues. |
| GitHub API down | The relay returns failure → red toast; nothing is queued or lost silently. |
