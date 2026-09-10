'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { Miniflare } = require(process.argv[2] ? path.resolve(process.argv[2]) : 'miniflare');

const root = path.join(__dirname, '..');
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'snapmap-plus-workers-'));
const sidA = 'a'.repeat(64), sidB = 'b'.repeat(64), sidC = 'c'.repeat(64);
const image = new Uint8Array(16);
image.set([0x89, 0x50, 0x4e, 0x47]);

function community() {
  return new Miniflare({
    name: 'community-runtime-test',
    modules: true, modulesRoot: root, scriptPath: path.join(root, 'community/worker.js'),
    modulesRules: [{ type: 'ESModule', include: ['**/*.js'] }],
    compatibilityDate: '2026-07-01',
    durableObjects: { WRITE_QUOTAS: { className: 'CommunityQuota', useSQLite: true } },
    durableObjectsPersist: path.join(scratch, 'quotas'),
    kvNamespaces: ['SESSIONS'], r2Buckets: ['MEDIA'],
    outboundService: () => { throw new Error('unexpected external request'); },
  });
}

async function seed(mf) {
  const sessions = await mf.getKVNamespace('SESSIONS');
  for (const [sid, id] of [[sidA, '42'], [sidB, '42'], [sidC, '99']]) {
    await sessions.put('session:' + sid, JSON.stringify({ id, token: 'local-test-token', login: 'tester' }));
  }
}

function upload(mf, sid) {
  return mf.dispatchFetch('http://local.test/media/upload', { method: 'POST', body: image,
    headers: { Authorization: 'Bearer ' + sid, Origin: 'https://doom-snapmap.github.io' } });
}

async function run() {
  let mf = community();
  try {
    await seed(mf);
    const replies = await Promise.all(Array.from({ length: 45 }, (_, i) => upload(mf, i % 2 ? sidA : sidB)));
    assert.equal(replies.filter(response => response.status === 200).length, 30);
    assert.equal(replies.filter(response => response.status === 429).length, 15);
    assert.ok(replies.every(response => response.headers.get('Access-Control-Allow-Origin') === 'https://doom-snapmap.github.io'));
    for (const response of replies) await response.arrayBuffer();
    const other = await upload(mf, sidC);
    assert.equal(other.status, 200);
    await other.arrayBuffer();
    const media = await mf.getR2Bucket('MEDIA');
    assert.equal((await media.list()).objects.length, 31);
  } finally { await mf.dispose(); }

  mf = community();
  try {
    await seed(mf);
    const retained = await upload(mf, sidA);
    assert.equal(retained.status, 429, 'quota was lost when the object restarted');
    await retained.arrayBuffer();
  } finally { await mf.dispose(); }

  let issueWrites = 0;
  const feedback = new Miniflare({
    name: 'feedback-runtime-test',
    modules: true, modulesRoot: root, scriptPath: path.join(root, 'feedback/worker.js'),
    modulesRules: [{ type: 'ESModule', include: ['**/*.js'] }],
    compatibilityDate: '2026-07-01', bindings: { GITHUB_TOKEN: 'local-test-token' },
    outboundService: async request => {
      const url = new URL(request.url);
      assert.equal(url.hostname, 'api.github.com');
      if (request.method === 'GET' && url.pathname.endsWith('/issues')) return Response.json([]);
      assert.equal(request.method, 'POST');
      assert.ok(url.pathname.endsWith('/issues'));
      const issue = await request.json();
      assert.equal(issue.title, '[Bug] Local runtime test');
      assert.ok(issue.labels.includes('vulkan'));
      issueWrites++;
      return Response.json({ number: 123 });
    },
  });
  try {
    const response = await feedback.dispatchFetch('http://local.test/report', { method: 'POST',
      body: JSON.stringify({ category: 'bug', title: 'Local runtime test', body: 'A synthetic report for the local runtime.',
                             version: 'v1.0.0', renderer: 'vulkan' }) });
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), { ok: true, mode: 'created', number: 123 });
    const oversized = await feedback.dispatchFetch('http://local.test/report', { method: 'POST', body: new Uint8Array(3 * 65536 + 1) });
    assert.equal(oversized.status, 413);
    await oversized.arrayBuffer();
    assert.equal(issueWrites, 1);
  } finally { await feedback.dispose(); }
  console.log('worker runtime: concurrent per-account quotas, restart persistence, R2 and feedback relay passed');
}

run().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  if (path.dirname(scratch) !== os.tmpdir() || fs.lstatSync(scratch).isSymbolicLink()) throw new Error('unexpected test directory');
  fs.rmSync(scratch, { recursive: true, force: true });
});
