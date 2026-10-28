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
  return (await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'))).default;
}

function requestFor(url, body, method = 'POST', extra = {}) {
  return new Request('https://service.test' + url, { method, body, duplex: 'half', headers: {
    Authorization: 'Bearer ' + 'a'.repeat(64), ...extra,
  } });
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
