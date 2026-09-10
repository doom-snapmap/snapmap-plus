'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');

const root = path.join(__dirname, '..');
const bodySource = fs.readFileSync(path.join(root, 'src/workers/request_body.js'), 'utf8');
const bodyURL = 'data:text/javascript;base64,' + Buffer.from(bodySource).toString('base64');
const shared = import(bodyURL);

function streamed(chunks, declared) {
  let pulls = 0, cancelled = false;
  const body = new ReadableStream({
    pull(controller) {
      if (pulls === chunks.length) controller.close();
      else controller.enqueue(chunks[pulls++]);
    },
    cancel() { cancelled = true; },
  }, { highWaterMark: 0 });
  const headers = new Headers(declared === undefined ? {} : { 'Content-Length': declared });
  return { body, headers, pulls: () => pulls, cancelled: () => cancelled };
}

async function loadWorker(name) {
  let source = fs.readFileSync(path.join(root, name, 'worker.js'), 'utf8');
  source = source.replace('../src/workers/request_body.js', bodyURL);
  // The Cloudflare-only class export is exercised by worker_runtime_test.js.
  source = source.replace("export { CommunityQuota } from './quota.js';", '');
  return (await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'))).default;
}

function requestFor(url, body, method = 'POST', extra = {}) {
  return new Request('https://service.test' + url, { method, body, duplex: 'half', headers: {
    Authorization: 'Bearer ' + 'a'.repeat(64), Origin: 'https://doom-snapmap.github.io', ...extra,
  } });
}

function communityEnv(allowed = true) {
  const calls = { names: [], puts: 0 };
  return {
    calls,
    SESSIONS: { get: async () => JSON.stringify({ id: '42', token: 'local-test-token', login: 'tester' }) },
    WRITE_QUOTAS: {
      idFromName(name) { calls.names.push(name); return name; },
      get: () => ({ consume: async () => ({ allowed }) }),
    },
    MEDIA: { async put() { calls.puts++; } },
  };
}

test('reader accepts exact bounds and counts many small chunks without chunk retention', async () => {
  const { readBody } = await shared;
  const input = streamed(Array.from({ length: 17000 }, () => new Uint8Array([65])));
  const result = await readBody(input, 17000);
  assert.equal(result.length, 17000);
  assert.ok(result.every(byte => byte === 65));
  assert.equal(input.cancelled(), false);
});

test('actual streamed overflow cancels with absent or understated Content-Length', async () => {
  const { readBody } = await shared;
  for (const declared of [undefined, '1']) {
    const input = streamed([new Uint8Array(20), new Uint8Array(1), new Uint8Array(10)], declared);
    await assert.rejects(readBody(input, 20), error => error.status === 413);
    assert.equal(input.cancelled(), true);
    assert.equal(input.pulls(), 2);
    assert.equal(input.body.locked, false);
  }
});

test('oversized and malformed declared lengths are refused before reading', async () => {
  const { readBody } = await shared;
  for (const [declared, status] of [['21', 413], ['9999999999999999999999', 413], ['-1', 400], ['1x', 400]]) {
    const input = streamed([new Uint8Array(1)], declared);
    await assert.rejects(readBody(input, 20), error => error.status === status);
    assert.equal(input.pulls(), 0);
    assert.equal(input.cancelled(), true);
  }
});

test('JSON rejects malformed UTF-8, malformed syntax and non-object values', async () => {
  const { readJsonObject } = await shared;
  const inputs = [new Uint8Array([123, 34, 120, 34, 58, 34, 0xff, 34, 125]),
    ...['{"x":', 'null', '[]', 'true', '3'].map(text => new TextEncoder().encode(text))];
  for (const bytes of inputs) {
    await assert.rejects(readJsonObject(streamed([bytes]), 100), error => error.status === 400);
  }
  const bytes = new TextEncoder().encode('{"text":"\u00e9"}');
  assert.deepEqual(await readJsonObject(streamed([bytes.slice(0, 10), bytes.slice(10)]), 100), { text: '\u00e9' });
});

test('read errors become client errors and release the stream lock', async () => {
  const { readBody } = await shared;
  const input = { headers: new Headers(), body: new ReadableStream({ pull() { throw new Error('disconnect'); } }) };
  await assert.rejects(readBody(input, 100), error => error.status === 400);
  assert.equal(input.body.locked, false);
});

test('all Community JSON writes reject excess bytes with CORS before an upstream write', async () => {
  const worker = await loadWorker('community');
  for (const [method, route] of [['POST', '/community/discussions'], ['POST', '/community/discussions/1/comments'],
    ['POST', '/community/reactions'], ['POST', '/community/preview'], ['PATCH', '/community/discussions/1'],
    ['PATCH', '/community/comments/example'], ['DELETE', '/community/discussions/1'], ['POST', '/auth/logout']]) {
    const env = communityEnv();
    const response = await worker.fetch(requestFor(route, new Uint8Array(384 * 1024 + 1), method), env);
    assert.equal(response.status, 413, method + ' ' + route);
    assert.equal(response.headers.get('Access-Control-Allow-Origin'), 'https://doom-snapmap.github.io');
    assert.deepEqual(env.calls.names, []);
  }
});

test('Community rejects malformed JSON and preserves session refusal', async () => {
  const worker = await loadWorker('community');
  for (const body of ['null', '[]', '{', new Uint8Array([0xff])]) {
    assert.equal((await worker.fetch(requestFor('/community/reactions', body), communityEnv())).status, 400);
  }
  const env = communityEnv();
  env.SESSIONS.get = async () => null;
  assert.equal((await worker.fetch(requestFor('/community/reactions', '{}'), env)).status, 401);
});

test('upload overflow cancels before R2 and valid media retains its API shape', async () => {
  const worker = await loadWorker('community');
  const input = streamed([new Uint8Array(8 * 1024 * 1024), new Uint8Array(1), new Uint8Array(10)], '1');
  const env = communityEnv();
  const response = await worker.fetch(requestFor('/media/upload', input.body, 'POST', { 'Content-Length': '1' }), env);
  assert.equal(response.status, 413);
  assert.equal(input.cancelled(), true);
  assert.equal(env.calls.puts, 0);
  const image = new Uint8Array(16);
  image.set([0x89, 0x50, 0x4e, 0x47]);
  const valid = await worker.fetch(requestFor('/media/upload', image), env);
  assert.equal(valid.status, 200);
  assert.match((await valid.json()).url, /^https:\/\/service.test\/media\/community\/tester\/[a-f0-9]{32}\.png$/);
  assert.equal(env.calls.puts, 1);
  assert.ok(env.calls.names.every(name => name === 'github:42'));
});

test('quota exhaustion remains 429 and missing coordination fails closed with 503', async () => {
  const worker = await loadWorker('community');
  assert.equal((await worker.fetch(requestFor('/community/reactions', '{}'), communityEnv(false))).status, 429);
  const env = communityEnv();
  delete env.WRITE_QUOTAS;
  assert.equal((await worker.fetch(requestFor('/community/reactions', '{}'), env)).status, 503);
});

test('legacy sessions resolve a stable account id before quota admission', async () => {
  const worker = await loadWorker('community');
  const env = communityEnv(false);
  env.SESSIONS.get = async () => JSON.stringify({ token: 'local-test-token', login: 'tester' });
  let saved;
  env.SESSIONS.put = async (key, value) => { saved = { key, user: JSON.parse(value) }; };
  const originalFetch = globalThis.fetch;
  let lookups = 0;
  globalThis.fetch = async (url, options) => {
    assert.equal(url, 'https://api.github.com/user');
    assert.equal(options.headers.Authorization, 'Bearer local-test-token');
    lookups++;
    return Response.json({ id: 42, login: 'tester' });
  };
  try {
    const response = await worker.fetch(requestFor('/community/reactions', '{}'), env);
    assert.equal(response.status, 429);
    assert.equal(lookups, 1);
    assert.equal(saved.user.id, '42');
    assert.equal(saved.key, 'session:' + 'a'.repeat(64));
    assert.deepEqual(env.calls.names, ['github:42']);
    globalThis.fetch = async () => new Response(null, { status: 401 });
    assert.equal((await worker.fetch(requestFor('/community/reactions', '{}'), env)).status, 503);
    assert.equal(env.calls.names.length, 1);
  } finally { globalThis.fetch = originalFetch; }
});

test('comment edits retain the user credential and GitHub ownership refusal', async () => {
  const worker = await loadWorker('community');
  const originalFetch = globalThis.fetch;
  let authorized = true;
  globalThis.fetch = async (url, options) => {
    assert.equal(url, 'https://api.github.com/graphql');
    assert.equal(options.headers.Authorization, 'Bearer local-test-token');
    const payload = JSON.parse(options.body);
    assert.deepEqual(payload.variables, { commentId: 'example', body: 'Edited comment' });
    return Response.json({ data: authorized ? { updateDiscussionComment: { comment: { id: 'example' } } } : {} });
  };
  try {
    const edit = () => worker.fetch(requestFor('/community/comments/example', '{"body":"Edited comment"}', 'PATCH'), communityEnv());
    assert.equal((await edit()).status, 200);
    authorized = false;
    assert.equal((await edit()).status, 403);
  } finally { globalThis.fetch = originalFetch; }
});

test('feedback keeps its character limit, bounds encoded bytes and rejects non-object reports', async () => {
  const worker = await loadWorker('feedback');
  for (const [body, status] of [['null', 400], ['[]', 400], ['{', 400], [new Uint8Array([0xff]), 400],
    [JSON.stringify({ website: 'trap', extra: 'x'.repeat(65536) }), 413]]) {
    assert.equal((await worker.fetch(requestFor('/report', body), {})).status, status);
  }
  const input = streamed([new Uint8Array(3 * 65536), new Uint8Array(1), new Uint8Array(10)]);
  assert.equal((await worker.fetch(requestFor('/report', input.body), {})).status, 413);
  assert.equal(input.cancelled(), true);
  const accepted = await worker.fetch(requestFor('/report', JSON.stringify({ website: 'trap', extra: '\u4e00'.repeat(60000) })), {});
  assert.equal(accepted.status, 200);
  assert.deepEqual(await accepted.json(), { ok: true, mode: 'created', number: 0 });
  const inherited = await worker.fetch(requestFor('/report', JSON.stringify({ category: 'constructor', title: 'Title', body: 'A valid-length description' })), {});
  assert.equal(inherited.status, 400);
});
