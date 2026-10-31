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

// The File menu's path readout names the one file to share when the last save wrote the export.
function element() {
  return {
    children: [], textContent: '', title: '', disabled: false, attributes: {},
    set innerHTML(value) { this.children = []; },
    appendChild(child) { this.children.push(child); return child; },
    setAttribute(name, value) { this.attributes[name] = value; },
    querySelector() { return element(); },
  };
}
const ids = {};
const statusContext = vm.createContext({
  document: {
    getElementById: id => (ids[id] = ids[id] || element()),
    createElement: () => element(),
    createTextNode: text => ({textContent: text}),
  },
});
const start = page.indexOf('  var RAWMAP_ARMED = false;');
const end = page.indexOf("  document.getElementById('menuRawmapLoad')");
assert.ok(start > 0 && end > start);
vm.runInContext(page.slice(start, end), statusContext);
function readout(status) {
  delete ids.menuRawmapPaths;
  vm.runInContext('renderRawmapStatus(' + JSON.stringify(status) + ')', statusContext);
  return ids.menuRawmapPaths.children.map(row => row.textContent).join('\n');
}
const base = {load: 'C:\\maps\\rawmap.json', save: 'C:\\maps\\rawmap.json', armed: 0, saveTarget: ''};
assert.doesNotMatch(readout(base), /share/i, 'a map without a saved export says nothing about sharing');
assert.match(readout(Object.assign({}, base, {lastZip: 'C:\\maps\\rawmap.zip', lastZipBytes: 900, lastMods: 2})),
             /To share this map, send rawmap\.zip: it carries the map and its 2 mods\. The \.json alone will not load for others\./);
assert.match(readout(Object.assign({}, base, {lastZip: 'C:\\maps\\rawmap.zip', lastZipBytes: 900, lastMods: 1})),
             /its 1 mod\./);
assert.match(readout(Object.assign({}, base, {lastZip: 'C:\\maps\\rawmap.zip', lastZipBytes: 0, lastMods: 1,
                                              lastZipError: 'a mod is not installed'})),
             /This map uses 1 mod, but rawmap\.zip was not written: a mod is not installed\./);
console.log('rawmap_menu_test: OK');
