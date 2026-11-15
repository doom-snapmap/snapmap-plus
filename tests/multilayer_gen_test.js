// multilayer_gen_test.js -- the embedded multilayer blend (src/backend/modprog_multilayer.c) and modprog_test's
// stand-in program (tests/modprog_fixture.c) are what their sources give: an edited shader cannot ship without its
// regenerated copy. Where no shader compiler is at hand (CI) the texts are compared and the SPIR-V is kept.
const { test } = require('node:test');
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const path = require('node:path');

test('the embedded multilayer blend matches its shader sources', () => {
  const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'tools', 'gen_multilayer.js'), '--check'], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stdout + r.stderr);
});
