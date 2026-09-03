# External services

Every service Snapmap+ depends on, what it costs, and what breaks without it. A fork needs this list to stand
the project up on its own accounts.

| Service | What it does | Plan | Credential | Stored in | Without it |
|---|---|---|---|---|---|
| GitHub Actions | Builds, tests, publishes releases | Free (public repo) | `GITHUB_TOKEN`, minted per run | GitHub | No CI, no releases |
| GitHub Pages | Serves the website | Free | none | — | No website |
| Cloudflare Workers | The feedback relay and the Community service | Free tier | the GitHub App key below | Cloudflare Worker secrets | In-app feedback and community posts fail |
| GitHub App (feedback) | Files feedback as issues, as a bot identity | Free | App private key | Cloudflare, **not** GitHub | Feedback cannot file issues |
| **Anthropic API** | **Drafts each release's changelog entry** | **Paid, about $0.05 per release** | **`ANTHROPIC_API_KEY`** | **GitHub Environment `changelog`** | **Entries fall back to a commit-list skeleton; releases still work** |

## Anthropic API — the only paid service

Used by [`prepare-release.yml`](../.github/workflows/prepare-release.yml), **once per release**, to draft the
`CHANGELOG.md` entry a maintainer then reviews and merges. If it is unavailable the workflow still opens its
pull request, carrying a raw commit list marked `NEEDS WRITING` for hand-rewriting. A drafting failure never
blocks a release.

### Blast radius

The feedback GitHub App — the project's other credential — is scoped to Issues alone, lives in Cloudflare
rather than GitHub, and mints roughly one-hour installation tokens, so its worst case is deletable spam
issues. An API key has neither capability scoping nor an intrinsic ceiling. The controls, in the order they
actually bound the loss:

1. **A monthly spend limit on a dedicated Anthropic workspace**, set in the Console. This is the only control
   that bounds the financial exposure, and it is deliberately outside GitHub.
2. **The `changelog` environment's deployment branch policy, restricted to `main`.** A GitHub Environment
   defaults to "All branches", and `workflow_dispatch` lets the caller pick the ref — the workflow file that
   *runs* is the one on the chosen ref. Without this policy the key is scoped to a declared job, not to
   reviewed code.
3. **Job splitting.** The job holding the key declares `permissions: {}`; the job with `contents: write`
   declares no environment and so cannot read the key. They never share a process.
4. The key is set as step-level `env:`, not job-wide, so no other step in that job sees it.
5. `persist-credentials: false` on that job's checkout.
6. `pip install --require-hashes` against [`tools/requirements.txt`](../tools/requirements.txt).
7. A ruleset on `main` requiring code-owner review, so a drafted entry cannot merge itself.

### Setting it up

1. In the Anthropic Console, create a workspace for this project and set a **monthly spend limit** on it.
2. Create an API key scoped to that workspace.
3. In GitHub: **Settings → Environments → New environment**, named `changelog`.
4. Under **Deployment branches and tags**, choose **Selected branches and tags** and add `main`. This step is
   load-bearing — see control 2 above.
5. Add the environment secret `ANTHROPIC_API_KEY`.
6. **Settings → Actions → General → Workflow permissions**: enable **Allow GitHub Actions to create and
   approve pull requests**, or the workflow cannot open its pull request.

### Rotation

Revoke the key in the Anthropic Console, create a replacement in the same workspace, and update the
`changelog` environment secret. No code change.

### Removal

Delete the environment secret. `prepare-release.yml` then produces skeleton entries for hand-writing, and
nothing else changes. Deleting the workflow removes the dependency entirely; `CHANGELOG.md` is then written
by hand and every consumer still works.
