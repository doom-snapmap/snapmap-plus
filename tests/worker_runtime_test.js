'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { Miniflare } = require(process.argv[2] ? path.resolve(process.argv[2]) : 'miniflare');

const root = path.join(__dirname, '..');
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'snapmap-plus-workers-'));

async function run() {
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
  console.log('worker runtime: feedback relay passed');
}

run().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  if (path.dirname(scratch) !== os.tmpdir() || fs.lstatSync(scratch).isSymbolicLink()) throw new Error('unexpected test directory');
  fs.rmSync(scratch, { recursive: true, force: true });
});
