/* feedback/worker.js -- the feedback relay: a Cloudflare Worker that turns the in-app "Send feedback"
 * dialog's anonymous POST into a labeled issue on this project's GitHub tracker.
 *
 * Why it exists: GitHub has no anonymous write path (every API write needs a token), and a token must
 * never ship inside a public binary. So the app POSTs here, and this relay -- holding a fine-grained
 * PAT scoped to ONE repo with Issues read/write only, stored as a Worker secret -- files the issue.
 *
 * Flow per report:
 *   validate (category / lengths / size) -> honeypot check -> compute a dedup signature ->
 *   search open issues for that signature:
 *     hit  -> append a comment to the existing issue (one issue with N confirmations, not N issues)
 *     miss -> create a new issue, labeled: category label + release channel + user-report
 *
 * Ops notes (see also feedback/README.md):
 *   - secret: `wrangler secret put GITHUB_TOKEN` (fine-grained PAT; expires -> annual rotation).
 *   - stateless by design: no KV, no queues; GitHub Issues is the only store.
 *   - abuse posture: honeypot + size caps here; if real spam ever shows up, add a Cloudflare
 *     rate-limiting rule on the dashboard (no code change needed).
 */

const REPO = 'snaphak/open-snaphak';
const API = 'https://api.github.com';

const CATEGORIES = {
  bug:     { label: 'bug',           tag: 'Bug' },
  feature: { label: 'enhancement',   tag: 'Feature' },
  docs:    { label: 'documentation', tag: 'Docs' },
  other:   { label: 'question',      tag: 'Other' },
};

function json(obj, status) {
  return new Response(JSON.stringify(obj), {
    status: status || 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

async function gh(env, path, opts) {
  const o = opts || {};
  const res = await fetch(API + path, {
    method: o.method || 'GET',
    headers: {
      'Authorization': 'Bearer ' + env.GITHUB_TOKEN,
      'Accept': 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      'User-Agent': 'snaphak-feedback-relay',
      ...(o.body ? { 'Content-Type': 'application/json' } : {}),
    },
    body: o.body ? JSON.stringify(o.body) : undefined,
  });
  if (!res.ok) return null;
  return res.json();
}

/* dedup signature: category + normalized title -> first 16 hex of SHA-256. Embedded in the issue body
 * as an HTML comment; exact-match only (fuzzy "same bug, different words" stays a human call). */
async function sigHash(category, title) {
  const norm = category + '|' + title.toLowerCase().trim().replace(/\s+/g, ' ');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(norm));
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 16);
}

/* release channel from the reported version: a "-" (e.g. 0.2.0-beta.1) means a pre-release build;
 * a plain x.y.z means stable; anything non-semver (a dev build) gets no channel label. */
function channelOf(version) {
  if (!/^\d+\.\d+\.\d+/.test(version)) return null;
  return version.includes('-') ? 'beta' : 'stable';
}

function issueBody(details, version, channel, contact, sig) {
  const meta = [
    '',
    '---',
    '- Version: ' + version + (channel ? ' (' + channel + ')' : ''),
  ];
  if (contact) meta.push('- Contact: ' + contact);
  meta.push('', '<!-- report-sig:' + sig + ' -->');
  meta.push('<sub>Filed automatically from the in-app feedback dialog.</sub>');
  return details + '\n' + meta.join('\n');
}

function commentBody(details, version, channel, contact) {
  const lines = ['Another report of this, on version ' + version + (channel ? ' (' + channel + ')' : '') + ':', '', details];
  if (contact) lines.push('', '- Contact: ' + contact);
  lines.push('', '<sub>Added automatically from the in-app feedback dialog (matching report signature).</sub>');
  return lines.join('\n');
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (req.method === 'GET' && url.pathname === '/') {
      return new Response('snaphak feedback relay: OK\n', { headers: { 'Content-Type': 'text/plain' } });
    }
    if (req.method !== 'POST' || url.pathname !== '/report') return json({ ok: false, error: 'not found' }, 404);

    const raw = await req.text();
    if (raw.length > 65536) return json({ ok: false, error: 'too large' }, 413);
    let body;
    try { body = JSON.parse(raw); } catch { return json({ ok: false, error: 'bad json' }, 400); }

    /* honeypot: a hidden field humans never see. A bot that filled it gets a convincing fake success
     * (nothing filed) -- a rejection would just teach it which field to skip. */
    if (body.website) return json({ ok: true, mode: 'created', number: 0 });

    const cat = CATEGORIES[body.category];
    const title = String(body.title || '').trim();
    const details = String(body.body || '').trim();
    const contact = String(body.contact || '').trim().slice(0, 200);
    const version = String(body.version || 'unknown').trim().slice(0, 40) || 'unknown';
    if (!cat) return json({ ok: false, error: 'bad category' }, 400);
    if (title.length < 3 || title.length > 120) return json({ ok: false, error: 'bad title' }, 400);
    if (details.length < 10 || details.length > 8000) return json({ ok: false, error: 'bad details' }, 400);

    const channel = channelOf(version);
    const sig = await sigHash(body.category, title);

    /* dedup (best-effort: a search failure falls through to plain create). Closed matches are NOT
     * resurrected -- closed means resolved or rejected; a fresh report opens a fresh issue. */
    const q = 'repo:' + REPO + ' is:issue is:open "report-sig:' + sig + '"';
    const found = await gh(env, '/search/issues?per_page=1&q=' + encodeURIComponent(q));
    if (found && found.total_count > 0 && found.items && found.items[0]) {
      const n = found.items[0].number;
      const c = await gh(env, '/repos/' + REPO + '/issues/' + n + '/comments', {
        method: 'POST',
        body: { body: commentBody(details, version, channel, contact) },
      });
      if (c) return json({ ok: true, mode: 'appended', number: n });
      /* comment failed -> fall through and file a fresh issue rather than dropping the report */
    }

    const labels = [cat.label, 'user-report'];
    if (channel) labels.push(channel);
    const issue = await gh(env, '/repos/' + REPO + '/issues', {
      method: 'POST',
      body: {
        title: '[' + cat.tag + '] ' + title,
        body: issueBody(details, version, channel, contact, sig),
        labels,
      },
    });
    if (!issue || !issue.number) return json({ ok: false, error: 'upstream' }, 502);
    return json({ ok: true, mode: 'created', number: issue.number });
  },
};
