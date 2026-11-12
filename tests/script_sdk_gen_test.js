// script_sdk_gen_test.js -- the embedded copy of the script SDK (src/backend/script/script_sdk_lua.c)
// is what src/sdk/lua/*.lua give: an edited library cannot ship without its regenerated copy.
const { test } = require('node:test');
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const path = require('node:path');

test('the embedded script SDK matches its Lua sources', () => {
  const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'tools', 'gen_script_sdk.js'), '--check'], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stdout + r.stderr);
});
