'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const page = fs.readFileSync(path.join(__dirname, '../src/ui/webview/mockup.html'), 'utf8');
const sent = [];
let confirm;
const context = vm.createContext({
  RAWMAP_LOAD_PATH: 'C:\\maps\\staged.json', RAWMAP_SAVE_PATH: 'C:\\maps\\previous-map.json',
  post: message => sent.push(message), closeMenus() {},
  rawmapBaseName: value => value.split('\\').pop(),
  showConfirm: (message, accept) => { confirm = {message, accept}; },
});
function click(id) {
  const marker = "document.getElementById('" + id + "').addEventListener('click', function(){";
  const start = page.indexOf(marker);
  assert.ok(start >= 0);
  const body = page.slice(start + marker.length, page.indexOf('\n  });', start));
  vm.runInContext(body, context);
}
click('menuRawmapReload');
assert.equal(sent.length, 0, 'opening a staged rawmap waits for confirmation');
assert.match(confirm.message, /staged\.json/);
assert.match(confirm.message, /unsaved edits/);
confirm.accept();
assert.equal(sent.pop().cmd, 'rawmapLoadNow');
click('menuRawmapSave');
assert.equal(sent[0].cmd, 'rawmapSaveNow');
assert.equal(sent[0].path, undefined, 'Save resolves its path in the backend, not a stale page snapshot');
console.log('rawmap_menu_test: OK');
