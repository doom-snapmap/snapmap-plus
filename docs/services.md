# External services

The services used by the repository and website, their credentials, and fork
setup requirements. Hosting charges depend on the account plan and usage;
release drafting makes a paid API call when its key is configured.

| Service | Purpose | Credentials or bindings | Without it |
|---|---|---|---|
| GitHub Actions | Builds, checks and releases | Per-run `GITHUB_TOKEN` | Automated checks and publishing stop. |
| GitHub Pages | Public website | Workflow Pages permissions | The site cannot publish. |
| Feedback Worker and GitHub App | Files reports as issues | Worker secrets `APP_ID`, `APP_PRIVATE_KEY`; optional `GITHUB_TOKEN` fallback | In-app reports cannot reach GitHub. |
| Community Worker and GitHub App | Reads Discussions and handles GitHub sign-in | Worker secrets `APP_ID`, `APP_PRIVATE_KEY`, `CLIENT_SECRET`; optional `GITHUB_TOKEN` read fallback | Community reads or sign-in fail. |
| Workers KV | Community sessions and OAuth state | `SESSIONS` binding | Sign-in and authenticated routes fail. |
| SQLite Durable Objects | Community write quotas per GitHub account | `WRITE_QUOTAS` binding and its migration | Authenticated writes fail closed with HTTP 503. |
| R2 | Community screenshots | `MEDIA` binding | Uploads and media retrieval fail. |
| Turnstile | Optional Community write challenge | Worker secret `TURNSTILE_SECRET` and matching site setup | The Worker skips the challenge when the secret is absent. |
| Anthropic API | Drafts release notes for review | GitHub Environment `changelog`: `ANTHROPIC_API_KEY` | The workflow proposes a commit-list skeleton. |

## Worker services

[Feedback setup](../feedback/README.md) covers the issue-writing App and its
private key. [Community setup](../community/README.md) covers the Discussions App,
OAuth callback, KV, Durable Objects and R2. Their App credentials live in Cloudflare, separately
from GitHub Actions secrets. The Community Worker stores signed-in users' GitHub
tokens in KV and sends browsers opaque session IDs; writes use the user's token.

Both Workers bound request bodies while reading the stream. Community write
quotas use one Durable Object per GitHub account, so simultaneous requests and
multiple sessions share the same persisted hourly limit. Deploy the binding and
migration in `community/wrangler.toml` with the Worker code. The local runtime
tests verify concurrent enforcement and persistence without writing to GitHub.

A fork must provision its own services and storage, configure the App repository
permissions, replace account-specific IDs and callback/origin constants, and
update the endpoint URLs in the frontend and website. Source comments and
`wrangler.toml` describe the checked-in configuration, not current account billing
or deployed permissions.

## Release drafting API

Used by [`prepare-release.yml`](../.github/workflows/prepare-release.yml), **once per release**, to draft the
`CHANGELOG.md` entry a maintainer then reviews and merges. If it is unavailable the workflow still opens its
pull request, carrying a raw commit list marked `NEEDS WRITING` for hand-rewriting. A drafting failure never
blocks a release.

### Blast radius

The drafting key authorizes billable requests. Keep its spending controls
separate from the repository's write permissions. The intended deployment uses:

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
