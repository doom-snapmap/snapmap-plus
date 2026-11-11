'use strict';
/* The long-values flow of prefab_mods_test against the mod store Worker's local runtime:
 *   node tests/long_values_worker_test.js <miniflare module> <snapmap-web checkout>
 * The Worker (snapmap-web api/worker.js) runs under Miniflare on a loopback port;
 * prefab_mods_test.exe (built by run-tests.ps1) then builds a mod with a 2,000-character
 * name and author, a 10,000-character homepage and a carried file 60 folders deep whose
 * request path is 5,000 bytes, in mods folders under a long path; archives and publishes
 * it to that Worker, saves and reads a map's rows naming it, downloads it into the map's
 * place and installs it, each value whole (SMP_LONG_ONLY, SMP_STORE_URL). */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const [miniflarePath, webRoot] = process.argv.slice(2);
if (!miniflarePath || !webRoot) {
  console.error('usage: node tests/long_values_worker_test.js <miniflare module> <snapmap-web checkout>');
  process.exit(2);
}
const { Miniflare } = require(path.resolve(miniflarePath));
const exe = path.join(__dirname, 'obj', 'prefab_mods_test.exe');
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'snapmap-plus-long-values-'));

async function run() {
  assert.ok(fs.existsSync(exe), 'build prefab_mods_test first (tests\\run-tests.ps1 -Only prefab_mods_test)');
  const root = path.resolve(webRoot);
  const mf = new Miniflare({
    name: 'long-values-store',
    modules: true, modulesRoot: root, scriptPath: path.join(root, 'api/worker.js'),
    modulesRules: [{ type: 'ESModule', include: ['**/*.js'] }],
    compatibilityDate: '2026-07-01',
    durableObjects: { WRITE_QUOTAS: { className: 'CommunityQuota', useSQLite: true },
                      MOD_REGISTRY: { className: 'ModRegistry', useSQLite: true } },
    durableObjectsPersist: path.join(scratch, 'objects'),
    kvNamespaces: ['SESSIONS'], r2Buckets: ['MEDIA', 'MODS'], bindings: { MODS_BLOCKED: '[]' },
    host: '127.0.0.1', port: 0,
    outboundService: () => { throw new Error('unexpected external request'); },
  });
  try {
    const url = String(await mf.ready).replace(/\/$/, '');
    const result = spawnSync(exe, [], { encoding: 'utf8', timeout: 600000,
      env: { ...process.env, SMP_LONG_ONLY: '1', SMP_STORE_URL: url } });
    process.stdout.write(result.stdout || '');
    process.stderr.write(result.stderr || '');
    assert.equal(result.status, 0, 'prefab_mods_test long-values flow against the Worker');
    assert.match(result.stdout, /against the store at http:\/\/127\.0\.0\.1:\d+/);
    assert.match(result.stdout, /a 5000-byte request 60 folders deep under a \d+-byte mods folder, a 2000-character name: carried whole/);
    const bucket = await mf.getR2Bucket('MODS');
    assert.ok((await bucket.list({ prefix: 'build/' })).objects.length >= 1, 'the Worker stored the build');
  } finally { await mf.dispose(); }
  console.log('long values: build, archive, publish to the Worker, map rows, download and install passed');
}

run().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  if (path.dirname(scratch) !== os.tmpdir() || fs.lstatSync(scratch).isSymbolicLink()) throw new Error('unexpected test directory');
  fs.rmSync(scratch, { recursive: true, force: true });
});
