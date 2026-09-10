# Feedback relay

## Purpose

Accepts reports from the in-app feedback and crash dialogs and files them as
GitHub issues. A Cloudflare Worker holds the repository credential so players
can submit a report without a GitHub account.

## Contents

- `worker.js` validates reports, groups matching open reports and creates issues
  or confirmation comments.
- `wrangler.toml` defines the Worker name, entry point and compatibility date.
- `../src/workers/request_body.js` bounds and validates incoming JSON.

## Working here

Read the [feedback pipeline](../docs/feedback.md) for the dialog-to-issue flow and
crash-log attachments. Keep the endpoint in
`src/ui/webview/snapmap_plus_ui_webview.cpp` aligned with the deployed Worker.
Credentials belong in Worker secrets. The [service inventory](../docs/services.md)
records ownership and the release-related credentials used elsewhere.

## Deployment

Use a Cloudflare account and Node.js. From this directory:

```text
npx wrangler login
npx wrangler deploy
```

The health endpoint is `GET /`; a working relay responds with
`snapmap-plus feedback relay: OK`. Configure credentials before testing report
submission, since a successful submission creates or comments on an issue.

## GitHub App setup

1. Create an organization-owned GitHub App. Disable webhooks and grant repository
   Issues read/write access. Install it only on the report repository.
2. Note its App ID and generate a private key.
3. Convert the downloaded key to PKCS#8:

   ```text
   openssl pkcs8 -topk8 -inform PEM -nocrypt -in downloaded.pem -out app-pkcs8.pem
   ```

4. Store `APP_ID` and `APP_PRIVATE_KEY` with `npx wrangler secret put`, including
   the complete PEM header and footer for the key. Remove local key copies when
   no longer needed.

The relay mints short-lived installation tokens and files reports as the App's
bot identity. Revoke and replace the App key if it is compromised. For a fork,
update the repository constants in `worker.js` as well.

Without the App credentials, the Worker can use a fine-grained PAT from its
`GITHUB_TOKEN` secret. Scope it to Issues read/write on the report repository;
reports then use that token owner's identity. Track its expiration and rotation.

## Validation and limits

The relay applies a honeypot, payload caps and exact report-signature matching.
It retains the 65,536-character JSON limit and caps incoming UTF-8 at 192 KiB
while streaming. Malformed JSON or UTF-8 returns 400; excess bytes or characters
return 413. Run `node tests/worker_body_test.js` from the repository root for
local validation; no credentials or report submission are needed.
A match appends only to an open issue; a closed report is not reopened. These
checks do not replace edge rate limiting or maintainer moderation. The credential
should remain scoped to report management.
