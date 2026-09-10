# Community service

## Purpose

Serves the website's Community pages through GitHub Discussions, handles GitHub sign-in and stores uploaded screenshots.

## Contents

- `worker.js` defines the HTTP API, authentication, discussion operations and media handling.
- `quota.js` and `quota_store.js` coordinate write limits across a user's sessions.
- `wrangler.toml` binds `SESSIONS` KV, `MEDIA` R2 and the SQLite `WRITE_QUOTAS` Durable Object.
- `../src/workers/request_body.js` bounds request bodies before parsing or uploading.

## Working here

Reads use a GitHub App installation token, with an optional `GITHUB_TOKEN` fallback. Writes use the signed-in user's token stored in KV; the browser receives an opaque session ID. Configure `APP_ID`, `APP_PRIVATE_KEY` (PKCS#8) and `CLIENT_SECRET` as Worker secrets. `TURNSTILE_SECRET` enables the optional challenge on supported write routes.

For a fork, provision your own KV namespace and R2 bucket, set the App callback URL to this Worker's `/auth/callback`, and update the repository, App client ID and site-origin constants. Keep the Worker URL in `site/assets/community.js` aligned. CORS admits the configured site and local HTTP development origins.

Writes share a fixed UTC-hour quota per GitHub account: 10 posts, 60 comments,
120 reactions, 30 uploads, 120 previews and 60 edits/deletions. Counters are atomic
and persist across restarts; signing in again does not reset them. Quota exhaustion
returns 429, and unavailable quota storage returns 503. The first deployment of
this configuration provisions `CommunityQuota` through its recorded migration;
deploy the binding and code together. KV is used for sessions and OAuth state.

JSON requests are limited to 384 KiB and images to 8 MiB of actual streamed bytes.
Malformed JSON or UTF-8 returns 400; oversize bodies return 413. Health is exposed
at `GET /`. Preview locally before a maintainer deploys. See the
[service inventory](../docs/services.md) and [website source](../site/README.md).

From the repository root, run `node tests/worker_body_test.js` and
`node tests/worker_quota_test.js` with Node 22.13 or newer. The optional local
Cloudflare runtime check is `node tests/worker_runtime_test.js <miniflare-module-path>`;
it uses Miniflare 4.20260730.0 and never calls GitHub or deployed storage.

## HTTP routes

| Method and path | Purpose |
|---|---|
| `GET /auth/login`, `/auth/callback`, `/auth/me` | Start sign-in, finish it, or read the current session. |
| `POST /auth/logout` | Remove the current session. |
| `GET /community/categories`, `/community/discussions`, `/community/search` | Browse and search Discussions. |
| `GET /community/discussions/:number` | Read a discussion and its comments. |
| `POST /community/discussions` | Create a discussion. |
| `POST /community/discussions/:number/comments` | Add a comment. |
| `PATCH` or `DELETE /community/discussions/:number` | Edit or delete an owned discussion. |
| `PATCH` or `DELETE /community/comments/:id` | Edit or delete an owned comment. |
| `POST /community/reactions`, `/community/preview` | React to content or preview Markdown. |
| `POST /media/upload`, `GET /media/:key` | Store or retrieve a screenshot. |

Write routes require a session. Read `worker.js` for payloads, limits and the
additional ownership or moderator checks on editing and deletion.
