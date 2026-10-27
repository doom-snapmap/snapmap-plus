# Feedback pipeline

How an in-app report becomes a tracked issue, end to end — and what keeps the tracker clean. The parts:
the **dialogs** (in the Studio UI: the "?" feedback dialog, and the crash-report dialog below), the
**relay** (a Cloudflare Worker, [`feedback/`](../feedback/README.md)), and two **hygiene workflows**
(GitHub Actions in this repo).

```
User clicks "?" -> Send-feedback dialog (category / title / details / optional contact)
  -> the UI host POSTs the report over HTTPS to the relay        (src/ui/webview/, one worker thread)
    -> the relay validates it and, holding the repo credential:
         same report already open?  -> appends a confirmation comment to that issue
         otherwise                  -> creates a new issue, labeled by category + release channel
  -> the user sees a green "Report sent" toast (or red on failure -- nothing typed is lost)
```

## Why a relay

GitHub has no anonymous write path — filing an issue requires a credential, and a credential must
never ship inside a public binary. The relay is the smallest possible fix: a stateless Worker that
accepts the app's unauthenticated POST and files the issue as the org's **GitHub App bot identity**
(installed on this repo with Issues read/write only — reports arrive from `<app-name>[bot]`, not a
personal account). The Worker caches short-lived installation tokens; maintainers
still own App-key rotation. Users need no account. Deployment and credentials:
[`feedback/README.md`](../feedback/README.md).

## What a filed issue looks like

The relay reads at most 192 KiB of UTF-8 bytes while streaming and keeps its
65,536-character JSON limit. It cancels oversized streams before parsing or
calling GitHub, including requests with an absent or understated Content-Length.
Invalid UTF-8, malformed JSON and non-object payloads return 400; excess bytes or
characters return 413. The UI receives a failed response and retains the draft.

- **Title:** `[Bug] <the user's title>` (or `[Feature]` / `[Docs]` / `[Other]`).
- **Body:** the user's details, then a metadata block: `- Version: 0.2.0-beta.3 (beta)`, a
  `- Renderer: Vulkan` (or `OpenGL`) line, a `- Game:` line, an optional `- Contact:` line, and an
  invisible `<!-- report-sig:… -->` marker (the dedup signature: a hash of category + normalized
  title). Nothing else is collected — no telemetry, and no system or hardware information beyond the
  renderer token and the game identity.
- **Labels:** the category (`bug` / `enhancement` / `documentation` / `question`), the release channel
  (`beta` / `stable`, from the version string; dev builds get neither), the renderer (`vulkan` /
  `opengl`), the game build (`build-13954591`, or `build-2642713` for a 2018-build report), and
  `user-report` (identifies relay-filed issues for release retest prompts).

A filed bug report's metadata block reads, for example:

```text
---
- Version: v0.2.2-beta.1 (beta)
- Renderer: Vulkan
- Game: DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)
- Contact: someone@example.com
```

**Why the game identity:** Snapmap+ supports exactly one DOOM 2016 build, so a report is only
actionable when it says which executable it came from. The backend produces one string from the
executable's SHA-256 fingerprint (`sh_host_game_identity`, `src/backend/host_image.c`): the Steam
build id, the binary name DOOM's console prints under Build Information, the renderer of that
executable, and the first eight hex digits of its hash. The same string appears in the backend log
(`game: …`), the diagnostic log, crash records, the installer's output and every report. The
backend passes it to the UI host in the startup argument block; when the backend did not attach to
a supported executable the UI has none, and the report says `- Game: not attached`. The relay
accepts only the identity strings of known builds (13954591, and 2642713 for legacy reports, each
with its binary name and per-renderer hash) and writes `- Game: unrecognized (not shown)` for
anything else rather than echoing it.

**Why the renderer:** DOOM 2016 ships one executable per renderer (`DOOMx64vk.exe` / `DOOMx64.exe`)
and relaunches itself when `r_renderAPI` changes, so a player can be in either one from the same Steam
launch. A bug that only reproduces under OpenGL is otherwise indistinguishable on the tracker from one
that happens under both. The app reports which renderer library the game process actually loaded —
not the executable name, and nothing else about the machine. The relay accepts only the two known
tokens; an unrecognized one is dropped rather than echoed into a public issue.

**Dedup:** the relay scans up to 300 open reports, oldest first. A matching
signature adds a confirmation comment. Lookup failures or concurrent submissions
can still create duplicates. Closed issues
are never resurrected (closed means resolved or rejected; a fresh report opens a fresh issue). Matching
is exact by design; judging that two differently-worded reports are the same bug stays a maintainer
call. The signature covers category + title only, so the same bug reported from both renderers stays
*one* issue whose confirmations show that it hits both.

## Crash reports

Fault capture attempts to write a local **crash record** containing the fault
class, exception code, `module+0xRVA`, call stack, available engine error,
timestamp, version, renderer and game identity to `<game>\snapmap-plus\crash\`.
Version, renderer and game identity are captured when the handlers are armed,
never at fault time. The record has no format version; readers treat a missing
`game` field (older records) as not recorded. A complete
temporary file is renamed into the pending set only after a successful write.
Capture is bounded and best effort: nested faults may be skipped, and some fatal
terminations bypass handlers entirely.

The Studio UI polls all retained filenames. Newly observed `classB` and
`offthread` records receive a notice, not a report dialog; those classifications
do not themselves prove that the process survived. Other records prompt for a
report in the current session when possible or on a later launch. Same-second
records are ordered by their numeric suffix. Fatal capture also attempts a local
dump at `snapmap-plus\logs\sh_crash.dmp`; dump creation can fail. A successful
dump's path appears in the record, but the dump is never uploaded automatically.

The dialog shows the bounded error and call stack that were recorded, takes an optional "what were
you doing?" description and optional contact, and offers an **Attach recent logs** checkbox (on by
default). Attached logs are the *tails* of the local log files, **anonymized before sending** — the
Windows account name, profile-folder name, and machine name are scrubbed out (`<user>` / `<machine>`).
Send files it through the relay as `category: crash`; Dismiss (or a successful send) clears the pending
record so the dialog never nags twice — the full logs and any dump stay on disk untouched.

What lands on the tracker: title `Crash: <module>+0x<rva> (0x<code>)` — the crash *location*, so the
signature lets the best-effort dedup group matching crashes — labels `crash` +
`user-report` + channel + renderer + `build-<id>`, a readable crash-summary body (including
`Game at crash: …`), and the attached logs as a
**collapsed follow-up comment**. Log attachment is best-effort: a failure after
issue creation does not change the successful submission response.

## Tracker hygiene (the two workflows)

Both use the built-in `GITHUB_TOKEN` with scoped permissions and pinned Actions.
Retest prompts require `user-report`; stale handling uses the waiting labels
below and does not require `user-report`. Pull requests are excluded.

- [`issues-retest.yml`](../.github/workflows/issues-retest.yml) — on every published release, open
  user reports filed on an **older** version get a "still reproducible on vX?" comment + the
  `needs-retest` label. A pre-release only prompts `beta`-channel reports; a stable release prompts
  everything older. A report is never prompted for a game build the new release does not run on: a
  release after v0.2.1-beta.19 supports only build 13954591, so reports whose `- Game:` line names
  another build, says `not attached` or is unrecognized are skipped, and reports filed without that
  line on v0.2.1-beta.19 or earlier count as the 2018 build (2642713). Deliberately not auto-close-on-release: an old-version bug usually still exists —
  real fixes are closed by a maintainer with "fixed in vX".
- [`issues-stale.yml`](../.github/workflows/issues-stale.yml) — nightly: issues labeled `needs-retest`
  or `awaiting-response` (i.e. waiting on their reporter) get a warning after 21 quiet days and close
  14 days later. Any issue with either waiting label is eligible, including a
  maintainer-filed issue; remove the label when a report no longer awaits a reply.

Label glossary: `user-report` (relay-filed), `crash` (filed by the crash-report dialog),
`beta`/`stable` (reported channel), `vulkan`/`opengl` (reported renderer), `build-<id>` (the DOOM
build the report came from: `build-13954591`, or `build-2642713` for the 2018 build),
`needs-retest` (superseded by a release, awaiting confirmation),
`awaiting-response` (maintainer asked the reporter a question — apply by hand to arm the stale sweep),
`stale` (the sweep's warning marker).

## Reporting by hand

Issues opened directly on GitHub (not through the app) should say which DOOM build they are on.
`snapmap-plus status` prints a `Game:` line per executable, in the same form the app reports
(`DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)`); paste that line into the issue, or name the build if Snapmap+ is not installed.

## Failure modes

| Failure | What happens |
|---|---|
| Relay down / offline / DNS | Red toast in-app ("could not send"); the dialog stays open with everything typed. |
| App key revoked / secrets misconfigured (or, on the PAT fallback, token expired) | Same red toast; fixed by re-storing the secrets — [`feedback/README.md`](../feedback/README.md). |
| Spam | Honeypot + size caps in the relay; escalation is a Cloudflare rate-limit rule (dashboard, no code). Worst case is deletable spam issues — the token can't touch anything but Issues. |
| Backend not attached (unsupported executable) | The UI is not started by an unsupported backend, so in practice reports always carry a game identity; a report without one says `- Game: not attached` and receives no `build-` label. |
| GitHub API down | Issue creation or confirmation failure returns an error. Dedup lookup failure can instead create a duplicate; a failed follow-up log attachment still reports success. |
