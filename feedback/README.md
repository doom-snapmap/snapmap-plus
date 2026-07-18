# feedback/ — the feedback relay

The in-app **Send feedback** dialog (the "?" button in SnapHak Studio) POSTs the user's report here —
a tiny Cloudflare Worker — and the relay files it as a labeled issue on this repo's tracker. Users never
need a GitHub account; the relay holds the only credential. See `worker.js` for the full flow
(validation → honeypot → dedup-by-signature → create or append) and `docs/feedback.md` for the
end-to-end pipeline including the repo-side hygiene workflows.

## Deploy (maintainer, one-time + on change)

Prereqs: a free Cloudflare account, Node.js.

```
cd feedback
npx wrangler login          # opens the browser, authorizes wrangler against your Cloudflare account
npx wrangler deploy         # prints the deployed URL: https://snaphak-feedback.<your-subdomain>.workers.dev
npx wrangler secret put GITHUB_TOKEN   # paste the fine-grained PAT (below) when prompted
```

Sanity check: `curl https://snaphak-feedback.<your-subdomain>.workers.dev/` → `snaphak feedback relay: OK`.

The deployed hostname must match `kReportHost` in `src/ui/webview/snaphak_ui_webview.cpp` — update it
there once after the first deploy (the URL is stable across redeploys).

## The token (and the annual rotation)

Create at github.com → Settings → Developer settings → Fine-grained personal access tokens:

- **Repository access:** only `snaphak/open-snaphak`.
- **Permissions:** Issues → Read and write. Nothing else.
- **Expiration:** 1 year (the maximum). Put the expiry date in your calendar.

Rotation (~2 minutes, once a year): generate a new token the same way, then
`cd feedback && npx wrangler secret put GITHUB_TOKEN` and paste it. Until rotated after expiry,
in-app reports fail with a red "could not send" toast — nothing else is affected.

## Abuse posture

Stateless and deliberately minimal: honeypot field + size/length caps in the Worker. If real spam ever
appears, add a Cloudflare **rate-limiting rule** on the dashboard (Security → WAF → Rate limiting rules,
scope it to `POST /report`) — no code change needed. Worst case is spam *issues*, which are deletable;
the token can't touch anything but Issues on this one repo.
