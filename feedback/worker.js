/* Relay in-app reports to GitHub Issues using a server-side App credential,
 * with a PAT fallback. Validate inputs, match an open report signature, then
 * append a confirmation or create an issue. GitHub is the only persistent store.
 * See README.md for setup and docs/feedback.md for the client pipeline. */

import { RequestError, readJsonObject } from '../src/workers/request_body.js';

const REPO = 'doom-snapmap/snapmap-plus';
const API = 'https://api.github.com';

const CATEGORIES = {
  bug:     { label: 'bug',           tag: 'Bug' },
  feature: { label: 'enhancement',   tag: 'Feature' },
  docs:    { label: 'documentation', tag: 'Docs' },
  other:   { label: 'question',      tag: 'Other' },
  /* Crash titles identify the fault location for signature matching. Each report
   * can attach anonymized log tails in a collapsed follow-up comment. */
  crash:   { label: 'crash',         tag: 'Crash' },
};

/* logs cap: well under GitHub's 65536-char comment limit once wrapped, and under the request-size
 * guard. The client already tails each log before sending. */
const LOGS_CAP = 50000;

/* Use a four-backtick fence so ordinary triple-backtick log text stays inside it. */
function logsBlock(logs) {
  return '<details><summary>Attached logs (anonymized)</summary>\n\n````text\n' + logs + '\n````\n</details>';
}

function json(obj, status) {
  return new Response(JSON.stringify(obj), {
    status: status || 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

async function gh(bearer, path, opts) {
  const o = opts || {};
  const res = await fetch(API + path, {
    method: o.method || 'GET',
    headers: {
      'Authorization': 'Bearer ' + bearer,
      'Accept': 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      'User-Agent': 'snapmap-plus-feedback-relay',
      ...(o.body ? { 'Content-Type': 'application/json' } : {}),
    },
    body: o.body ? JSON.stringify(o.body) : undefined,
  });
  if (!res.ok) return null;
  return res.json();
}

/* Exchange an App-signed JWT for a short-lived installation token and cache it
 * in this isolate. The private key stays in Worker secrets. */
let tokenCache = { token: null, exp: 0 };

function b64u(bytes) {
  return btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
async function appJwt(env) {
  const now = Math.floor(Date.now() / 1000);
  const enc = new TextEncoder();
  const head = b64u(enc.encode(JSON.stringify({ alg: 'RS256', typ: 'JWT' })));
  /* trim: a secret piped in via a shell often carries a trailing newline; GitHub rejects an iss with one */
  const pay = b64u(enc.encode(JSON.stringify({ iat: now - 60, exp: now + 540, iss: String(env.APP_ID).trim() })));
  const pem = env.APP_PRIVATE_KEY.replace(/-----[^-]+-----/g, '').replace(/\s+/g, '');
  const der = Uint8Array.from(atob(pem), c => c.charCodeAt(0));
  const key = await crypto.subtle.importKey('pkcs8', der, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['sign']);
  const sig = await crypto.subtle.sign('RSASSA-PKCS1-v1_5', key, enc.encode(head + '.' + pay));
  return head + '.' + pay + '.' + b64u(sig);
}
async function authToken(env) {
  if (!(env.APP_ID && env.APP_PRIVATE_KEY)) return env.GITHUB_TOKEN || null;   // PAT fallback
  const now = Date.now() / 1000;
  if (tokenCache.token && now < tokenCache.exp - 120) return tokenCache.token;
  let jwt;
  try { jwt = await appJwt(env); }
  catch (e) { console.log('app auth: jwt build failed:', e.message); return null; }
  const hdrs = {
    'Authorization': 'Bearer ' + jwt, 'Accept': 'application/vnd.github+json',
    'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'snapmap-plus-feedback-relay',
  };
  const instRes = await fetch(API + '/repos/' + REPO + '/installation', { headers: hdrs });
  if (!instRes.ok) { console.log('app auth: installation lookup ->', instRes.status, (await instRes.text()).slice(0, 200)); return null; }
  const inst = await instRes.json();
  const tokRes = await fetch(API + '/app/installations/' + inst.id + '/access_tokens', { method: 'POST', headers: hdrs });
  if (!tokRes.ok) { console.log('app auth: token exchange ->', tokRes.status, (await tokRes.text()).slice(0, 200)); return null; }
  const tok = await tokRes.json();
  tokenCache = { token: tok.token, exp: now + 3300 };   // installation tokens live ~1h
  return tok.token;
}

/* dedup signature: category + normalized title -> first 16 hex of SHA-256. Embedded in the issue body
 * as an HTML comment; exact-match only (fuzzy "same bug, different words" stays a human call). */
async function sigHash(category, title) {
  const norm = category + '|' + title.toLowerCase().trim().replace(/\s+/g, ' ');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(norm));
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 16);
}

/* Accept an optional v prefix on release tags. Hyphenated versions are beta;
 * plain versions are stable; unrecognized versions receive no channel label. */
function channelOf(version) {
  if (!/^v?\d+\.\d+\.\d+/.test(version)) return null;
  return version.includes('-') ? 'beta' : 'stable';
}

/* Accept only known renderer tokens; omit missing or unrecognized values from issues. */
const RENDERERS = { vulkan: 'Vulkan', opengl: 'OpenGL' };
function rendererOf(renderer) {
  return Object.prototype.hasOwnProperty.call(RENDERERS, renderer) ? renderer : null;
}

function issueBody(details, version, channel, contact, renderer, sig) {
  const meta = [
    '',
    '---',
    '- Version: ' + version + (channel ? ' (' + channel + ')' : ''),
  ];
  if (renderer) meta.push('- Renderer: ' + RENDERERS[renderer]);
  if (contact) meta.push('- Contact: ' + contact);
  meta.push('', '<!-- report-sig:' + sig + ' -->');
  meta.push('<sub>Filed automatically from the in-app feedback dialog.</sub>');
  return details + '\n' + meta.join('\n');
}

function commentBody(details, version, channel, contact, renderer, logs) {
  const lines = ['Another report of this, on version ' + version + (channel ? ' (' + channel + ')' : '') +
                 (renderer ? ', ' + RENDERERS[renderer] : '') + ':', '', details];
  if (contact) lines.push('', '- Contact: ' + contact);
  if (logs) lines.push('', logsBlock(logs));
  lines.push('', '<sub>Added automatically from the in-app feedback dialog (matching report signature).</sub>');
  return lines.join('\n');
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (req.method === 'GET' && url.pathname === '/') {
      return new Response('snapmap-plus feedback relay: OK\n', { headers: { 'Content-Type': 'text/plain' } });
    }
    if (req.method !== 'POST' || url.pathname !== '/report') return json({ ok: false, error: 'not found' }, 404);

    let body;
    try { body = await readJsonObject(req, 3 * 65536, 65536); }
    catch (error) {
      return json({ ok: false, error: error instanceof RequestError ? error.message : 'bad request body' },
                  error instanceof RequestError ? error.status : 400);
    }

    /* A filled honeypot returns success without filing a report. */
    if (body.website) return json({ ok: true, mode: 'created', number: 0 });

    const cat = Object.hasOwn(CATEGORIES, body.category) ? CATEGORIES[body.category] : null;
    const title = String(body.title || '').trim();
    const details = String(body.body || '').trim();
    const contact = String(body.contact || '').trim().slice(0, 200);
    const version = String(body.version || 'unknown').trim().slice(0, 40) || 'unknown';
    const renderer = rendererOf(String(body.renderer || '').trim().toLowerCase());
    /* optional log attachment (crash reports only -- ignored for the other categories). */
    const logs = body.category === 'crash' ? String(body.logs || '').slice(0, LOGS_CAP).trim() : '';
    if (!cat) return json({ ok: false, error: 'bad category' }, 400);
    if (title.length < 3 || title.length > 120) return json({ ok: false, error: 'bad title' }, 400);
    if (details.length < 10 || details.length > 8000) return json({ ok: false, error: 'bad details' }, 400);

    const channel = channelOf(version);
    const sig = await sigHash(body.category, title);
    const token = await authToken(env);
    if (!token) return json({ ok: false, error: 'relay auth' }, 500);

    /* Scan up to 300 open reports oldest-first, avoiding the search index delay.
     * Lookup failures and concurrent requests can still create duplicates.
     * Closed reports are never reopened. */
    let match = null;
    const marker = 'report-sig:' + sig;
    for (let page = 1; page <= 3 && !match; page++) {
      const batch = await gh(token, '/repos/' + REPO + '/issues?state=open&labels=user-report&sort=created&direction=asc&per_page=100&page=' + page);
      if (!batch || !batch.length) break;
      match = batch.find(i => !i.pull_request && (i.body || '').includes(marker)) || null;
      if (batch.length < 100) break;
    }
    if (match) {
      const n = match.number;
      const c = await gh(token, '/repos/' + REPO + '/issues/' + n + '/comments', {
        method: 'POST',
        body: { body: commentBody(details, version, channel, contact, renderer, logs) },
      });
      if (c) return json({ ok: true, mode: 'appended', number: n });
      /* comment failed -> fall through and file a fresh issue rather than dropping the report */
    }

    const labels = [cat.label, 'user-report'];
    if (channel) labels.push(channel);
    if (renderer) labels.push(renderer);
    const issue = await gh(token, '/repos/' + REPO + '/issues', {
      method: 'POST',
      body: {
        title: '[' + cat.tag + '] ' + title,
        body: issueBody(details, version, channel, contact, renderer, sig),
        labels,
      },
    });
    if (!issue || !issue.number) return json({ ok: false, error: 'upstream' }, 502);
    /* Attach logs separately after issue creation. Failure to add this comment
     * does not change the successful report response. */
    if (logs) {
      await gh(token, '/repos/' + REPO + '/issues/' + issue.number + '/comments', {
        method: 'POST',
        body: { body: 'Logs from the reporting session:\n\n' + logsBlock(logs) },
      });
    }
    return json({ ok: true, mode: 'created', number: issue.number });
  },
};
