# Community service

## Purpose

Serves the website's Community pages through GitHub Discussions, handles GitHub sign-in and stores uploaded screenshots.

## Contents

- `worker.js` defines the HTTP API, authentication, discussion operations and media handling.
- `wrangler.toml` selects the Worker entry point and binds `SESSIONS` KV and `MEDIA` R2.

## Working here

Reads use a GitHub App installation token, with an optional `GITHUB_TOKEN` fallback. Writes use the signed-in user's token stored in KV; the browser receives an opaque session ID. Configure `APP_ID`, `APP_PRIVATE_KEY` (PKCS#8) and `CLIENT_SECRET` as Worker secrets. `TURNSTILE_SECRET` enables the optional challenge on supported write routes.

For a fork, provision your own KV namespace and R2 bucket, set the App callback URL to this Worker's `/auth/callback`, and update the repository, App client ID and site-origin constants. Keep the Worker URL in `site/assets/community.js` aligned. CORS admits the configured site and local HTTP development origins.

The service uses short-lived per-isolate read caches and KV counters for approximate write throttling. Those counters are not atomic global limits. Health is exposed at `GET /`. Preview locally with Wrangler before a maintainer deploys from this directory. See the [service inventory](../docs/services.md) and [website source](../site/README.md).

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
