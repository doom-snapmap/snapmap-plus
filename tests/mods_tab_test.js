/* The Mods tab (mods_tab.js): the payload model (v3, and v2 from a backend that predates it), the
   three row states, the flat list's order and its one glyph per row, the retired collapsed-groups
   setting left alone, the origin line, the attention filter, the details
   sections, the empty states, the map modal (the decl focus modal's mechanism), the dry-run-driven
   confirmations of Remove from map and Uninstall, the vanilla state, the stand-in host the browser
   preview uses, and the page and host contract around it. tests/fixtures/mods_tab_payload.json is the
   v2 document an older backend sends; mods_tab_payload_v3.json is the v3 document the backend writes
   (its keys are checked by modui_test.c against the backend) and the browser preview's stand-in data. A minimal
   DOM stands in for the browser. Pure ASCII. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');
const web = path.join(root, 'src', 'ui', 'webview');
const modsTab = require(path.join(web, 'mods_tab.js'));
const v2 = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', 'mods_tab_payload.json'), 'utf8'));
const v3 = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', 'mods_tab_payload_v3.json'), 'utf8'));
const html = fs.readFileSync(path.join(web, 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const source = fs.readFileSync(path.join(web, 'mods_tab.js'), 'utf8');
const host = fs.readFileSync(path.join(web, 'snapmap_plus_ui_webview.cpp'), 'utf8').replace(/\r\n/g, '\n');
const iface = fs.readFileSync(path.join(root, 'src', 'common', 'snapmap_plus_iface.h'), 'utf8').replace(/\r\n/g, '\n');
const css = fs.readFileSync(path.join(web, 'studio.css'), 'utf8').replace(/\r\n/g, '\n');
const docs = fs.readFileSync(path.join(root, 'docs', 'webview-ui.md'), 'utf8').replace(/\r\n/g, '\n');
let passed = 0;
function test(name, fn) {
  try { fn(); } catch (e) { e.message = name + ': ' + e.message; throw e; }
  passed++;
}
const clone = v => JSON.parse(JSON.stringify(v));

/* ---- a minimal DOM ---------------------------------------------------------------------------- */
let activeElement = null;
class FakeNode {
  constructor(tag) {
    this.tagName = tag; this.children = []; this.parentNode = null; this.attrs = {}; this.style = {};
    this.listeners = {}; this.text = ''; this.className = ''; this.scrollTop = 0; this.clientHeight = 0;
    this.disabled = false; this.title = ''; this.value = ''; this.scrolledIntoView = 0; this.type = ''; this.id = '';
  }
  get firstChild() { return this.children[0] || null; }
  appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this.children.push(c); return c; }
  removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) { this.children.splice(i, 1); c.parentNode = null; } return c; }
  set textContent(v) { this.children.forEach(c => { c.parentNode = null; }); this.children = []; this.text = String(v); }
  get textContent() { return [this.text].concat(this.children.map(c => c.textContent)).filter(Boolean).join(' '); }
  set innerHTML(v) { throw new Error('mods_tab.js must not write innerHTML'); }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  dispatch(t, extra) {
    const e = Object.assign({type: t, target: this, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, stopPropagation() {}}, extra || {});
    (this.listeners[t] || []).forEach(f => f.call(this, e));
    return e;
  }
  click() { if (!this.disabled) this.dispatch('click'); }
  focus() { activeElement = this; }
  scrollIntoView() { this.scrolledIntoView++; }
  all(cls) {
    const out = [];
    const walk = n => { n.children.forEach(c => { if ((' ' + c.className + ' ').indexOf(' ' + cls + ' ') >= 0) out.push(c); walk(c); }); };
    walk(this);
    return out;
  }
  tags(tag) {
    const out = [];
    const walk = n => { n.children.forEach(c => { if (c.tagName === tag) out.push(c); walk(c); }); };
    walk(this);
    return out;
  }
}
const IDS = ['modsList', 'modsStatus', 'modsBadge', 'modsAttnBtn', 'modsRoot', 'modsDetail', 'modsFilter', 'modsInstallBtn',
             'modsFolderBtn', 'modsMigrateBtn', 'modsUninstallBtn', 'modsRebuildBtn', 'modsVerifyBtn', 'modsRefreshBtn',
             'modsMapBtn', 'modsMapModal', 'modsMapHost', 'modsUseBtn', 'modsFilesBtn', 'modsFilesModal', 'modsFilesHost',
             'modsNewModal', 'modsNewName', 'modsNewNote', 'modsNewCancel', 'modsNewCreate'];
const GONE = ['modsRemoveBtn', 'modsMap', 'modsMapHead', 'modsDetailHead', 'modsRescanBtn', 'modsVerifyOneBtn', 'modsPaletteBtn',
              'modsPruneBtn', 'modsShadows', 'modsRemoved', 'modsRemovedText', 'modsEmptyBtn'];
/* store: a stand-in host (modsTab.standIn) that answers every modsAction synchronously, as the
   browser preview does; without one the test answers by hand. */
function harness(opts) {
  opts = opts || {};
  const nodes = {};
  IDS.forEach(id => { nodes[id] = new FakeNode(id === 'modsAttnBtn' || /Btn$/.test(id) ? 'button' : 'div'); nodes[id].id = id; });
  nodes.modsMigrateBtn.style.display = 'none';
  nodes.modsMapModal.style.display = 'none';
  nodes.modsMapModal.appendChild(nodes.modsMapHost);
  nodes.modsFilesModal.style.display = 'none';
  nodes.modsFilesModal.appendChild(nodes.modsFilesHost);
  nodes.modsNewModal.style.display = 'none';
  const sent = [], toasts = [], menus = [];
  const saved = [];   /* what a page that still wrote mods.collapsed_groups would have saved */
  const doc = {createElement: tag => new FakeNode(tag), createElementNS: (ns, tag) => Object.assign(new FakeNode(tag), {ns})};
  let ui = null;
  ui = modsTab.create({
    document: doc, byId: id => nodes[id] || null, saveCollapsed: list => saved.push(list),
    post: m => { sent.push(m); if (opts.store) opts.store(m).forEach(r => ui.onMessage(r)); },
    toast: (t, k) => toasts.push({text: t, kind: k}),
    tokens: opts.tokens, menu: (x, y, items) => menus.push({x, y, items})
  });
  ui.bind();
  return {ui, nodes, sent, toasts, saved, menus};
}
function rows(h) { return h.nodes.modsList.all('mods-row'); }
function rowOf(h, key) { return rows(h).find(n => n.getAttribute('data-key') === key); }
function keys(h) { return rows(h).map(n => n.getAttribute('data-key')); }
/* The rows whose one glyph is "in this map" (the pill is gone, owner 2026-11-09). */
function pills(h) { return rows(h).filter(n => iconOf(n) === 'map').map(n => n.getAttribute('data-key')); }
function iconOf(n) { return n.all('mods-status-icon')[0].getAttribute('data-status'); }
function section(h, name, box) { return (box || h.nodes.modsDetail).all('mods-section').find(n => n.getAttribute('data-section') === name); }
function modalText(h) { return h.nodes.modsMapHost.textContent; }
function modalButtons(h) { return h.nodes.modsMapHost.tags('button').filter(b => !/mods-map-close/.test(b.className)); }
/* As the host answers a rescan: its result, then the document. */
function answer(h, data) {
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', data: {action: 'rescan', ok: true, message: 'Mods folder reread.'}});
  h.ui.onMessage({kind: 'mods', data: data});
}
function shown(h, data) { h.ui.show(); if (!h.ui.state().loaded) answer(h, data || v3); }
function live(data) { const h = harness({store: modsTab.standIn(data || v3)}); h.ui.show(); return h; }

/* ---- the model -------------------------------------------------------------------------------- */
test('normalize: the v3 fixture is the owner store', () => {
  const m = modsTab.normalize(v3);
  assert.equal(m.ok, true);
  assert.equal(m.v, 3);
  assert.equal(m.mods.length, 36);
  const hg = m.mods.find(b => b.key === 'local.hell-guard');
  assert.equal(hg.version, '0.0.0');
  assert.equal(hg.build, 2);
  assert.equal(hg.files - hg.pointers, 24);
  assert.equal(hg.pointers, 476);
  assert.equal(hg.counts.shadows, 498);   /* its two edited copies are edits, not replaced stock files */
  assert.equal(hg.origin, 'migration');
  assert.equal(hg.migratedFrom, 'overrides\\hell-guards');
  assert.deepEqual(hg.cvars, [{name: 'g_useImageBlackList', value: '0'}, {name: 'g_useResourceBlackList', value: '0'}]);
  assert.equal(hg.bundle.length, 64);
  assert.equal(m.mods.find(b => b.key === 'female.models').origin, 'delivery');
  assert.equal(m.mods.find(b => b.key === 'alex.r2-imp').origin, '');
  assert.equal(m.map.open, true);
  assert.equal(m.map.name, 'Hell Guard Arena');
  assert.equal(m.map.mods.length, 5);
  assert.equal(m.map.mods[0].placedCount, 12);
  assert.deepEqual(m.migrate, {entries: 34, pending: 2, running: false});
});
test('normalize: v2 leaves every v3 field unreported', () => {
  const m = modsTab.normalize(v2);
  assert.equal(m.mods.length, 7);
  const imp = m.mods.find(b => b.key === 'alex.r2-imp@1.0.0+2');
  assert.equal(imp.counts, null);
  assert.equal(imp.cvars, null);
  assert.equal(imp.editorRowNames, null);
  assert.equal(imp.carriedPaths, null);
  assert.equal(imp.publishedMaps, -1);
  assert.equal(imp.origin, '');
  assert.equal(imp.bundle, '');
  assert.equal(m.map.open, true, 'a v2 map with entities is open');
  assert.equal(m.map.mods, null);
  assert.equal(m.map.published, null);
  assert.equal(modsTab.normalize(Object.assign(clone(v2), {map: {entities: -1, names: 0, rows: [], placed: []}})).map.open, false);
});
test('normalize: hostile and missing values', () => {
  const bad = modsTab.normalize(null);
  assert.equal(bad.ok, false);
  assert.match(bad.error, /unavailable/);
  assert.equal(modsTab.normalize([]).ok, false);
  const m = modsTab.normalize({mods: [{key: 'x', state: 'exploded', size: 'lots', duplicateId: 'yes', origin: 'mars', counts: [1],
                                       requirements: {cvars: 'no'}, carriedPaths: 'a', publishedMaps: null}],
                               migrate: 'soon', storeReadable: false, map: 'x'});
  const b = m.mods[0];
  assert.equal(b.state, 'attention');
  assert.equal(b.size, 0);
  assert.equal(b.duplicateId, false);
  assert.equal(b.origin, '');
  assert.equal(b.counts, null);
  assert.equal(b.cvars, null);
  assert.equal(b.carriedPaths, null);
  assert.equal(b.publishedMaps, -1);
  assert.equal(m.map, null);
  assert.deepEqual(m.migrate, {entries: 0, pending: 0, running: false});
  assert.match(m.error, /cannot be read/);
  assert.equal(modsTab.normPreview(null), null);
  assert.equal(modsTab.normPreview({action: 'uninstall', install: {disposition: 'shred'}}).install.disposition, 'recycle');
  const pv = modsTab.normPreview({action: 'mapRemoveMod', map: {references: [{kind: 'toybox', name: 'a'}, {kind: 'module', name: 'b'},
                                  {kind: 'wire', name: 'c'}, {kind: 'timeline', name: 'd'}]}});
  assert.deepEqual(pv.map.references.map(r => r.kind), ['wire', 'timeline'], 'only wires and timeline references are severed');
  assert.deepEqual(pv.editorRows, []);
});
test('two states: every backend state and cause maps onto check or triangle, with no build talk', () => {
  const s = b => modsTab.statusInfo(Object.assign({state: 'installed', files: 3, cause: '', duplicateId: false, servedBy: ''}, b));
  assert.deepEqual([s({}).tone, s({}).icon], ['ok', 'check']);
  assert.equal(s({}).sentence, 'Installed and serving 3 files');
  assert.equal(s({files: 1}).sentence, 'Installed and serving 1 file');
  assert.equal(s({files: 500, pointers: 476}).sentence, 'Installed and serving 24 files, 476 more read from your install',
    'carried files only (files minus pointers), the pointers as a second clause');
  assert.equal(modsTab.servingText({files: 2, pointers: 1}), 'Installed and serving 1 file, 1 more read from your install');
  const dup = s({state: 'attention', duplicateId: true});
  assert.deepEqual([dup.tone, dup.icon], ['warn', 'triangle-alert']);
  assert.equal(dup.sentence, 'Needs attention: ' + modsTab.DUPLICATE_TEXT);
  assert.equal(s({duplicateId: true, servedBy: 'x'}).tone, 'warn', 'attention wins over a superseded build');
  assert.ok(!/change one id/.test(modsTab.DUPLICATE_TEXT), 'a key never changes (DESIGN-additive 2.2)');
  assert.match(modsTab.DUPLICATE_TEXT, /identical copy/);
  for (const cause of ['not in the serving index yet; it is used after the next scan', 'skipped by the serving index',
                       'not built yet: bundle.json is missing', 'mod.json unreadable'])
    assert.equal(s({state: 'attention', cause}).cause, cause);
  assert.equal(s({state: 'attention'}).cause, 'not in use');
  /* a folder another build of its key outranks is simply installed (owner 2026-11-09) */
  const info = s({servedBy: 'local.cyberdemon@0.0.0-05238b2e'});
  assert.deepEqual([info.tone, info.icon], ['ok', 'check']);
  assert.equal(info.sentence, 'Installed and serving 3 files');
  assert.deepEqual(Object.keys(modsTab.STATUS), ['ok', 'warn']);
  assert.equal(modsTab.mapModStatus({state: 'missing'}).cause, 'not installed on this computer; playing the map downloads it');
  assert.equal(modsTab.mapModStatus({state: 'otherBuild'}).cause, modsTab.mapModStatus({state: 'missing'}).cause,
    'another installed build of the key is never mentioned');
  assert.equal(modsTab.mapModStatus({state: 'active'}).tone, 'ok');
  assert.equal(modsTab.mapModStatus({state: 'superseded'}).tone, 'ok');
  const failed = s({buildFailed: {cause: 'x.decl: parse error', snapshot: true, servingBuild: 3}});
  assert.equal(failed.cause, 'its latest build failed: x.decl: parse error; the last working version keeps serving until it builds');
  for (const t of [info.sentence, failed.cause, modsTab.DUPLICATE_TEXT, modsTab.mapModStatus({state: 'otherBuild'}).cause])
    assert.doesNotMatch(t, /build \d|another build|upgrade|legacy|grandfather/i, t);
});
test('origins: one plain line per origin', () => {
  assert.equal(modsTab.originText({origin: ''}), 'Added by hand');
  assert.equal(modsTab.originText({origin: 'install'}), 'Imported from an export');
  assert.equal(modsTab.originText({origin: 'migration', migratedFrom: 'overrides\\hell-guards'}),
    'Moved from the old overrides folder (overrides\\hell-guards)');
  assert.equal(modsTab.originText({origin: 'migration', migratedFrom: ''}), 'Moved from the old overrides folder');
  assert.equal(modsTab.originText({origin: 'download'}), 'Downloaded for a map you played');
  assert.equal(modsTab.renamedText({renamedFrom: 'cyberprobe', renamedReferences: 3}),
               'Renamed from cyberprobe; 3 references rewritten');
  assert.equal(modsTab.renamedText({renamedFrom: 'probe', renamedReferences: 1}), 'Renamed from probe; 1 reference rewritten');
  assert.equal(modsTab.renamedText({renamedFrom: '', renamedReferences: 0}), '');
  assert.equal(modsTab.originText({origin: 'delivery'}), 'Downloaded for a map you played');
  assert.equal(modsTab.originText({origin: 'mars'}), 'Added by hand');
  assert.equal(modsTab.GROUPS, undefined, 'the origin groups are gone');
  assert.equal(modsTab.groupOf, undefined);
});
test('the list order: display name without case, then id, then folder', () => {
  const m = modsTab.normalize({mods: [
    {key: 'b2', id: 'z.same', name: 'Same'}, {key: 'b1', id: 'a.same', name: 'same'}, {key: 'c', id: 'c.zeta', name: 'zeta'},
    {key: 'a', id: 'a.alpha', name: 'Alpha'}, {key: 'n', id: 'beta.noname', name: ''}, {key: 'd2', id: 'd.dup', name: 'Dup'},
    {key: 'd1', id: 'd.dup', name: 'Dup'}]});
  assert.deepEqual(m.mods.map(b => b.key), ['a', 'n', 'd1', 'd2', 'b1', 'b2', 'c'],
    'Alpha, beta.noname (its id is its name), Dup by folder, same before Same by id, zeta');
  /* the v3 fixture: every one of the 36 folders in one sorted run */
  const v = modsTab.normalize(v3).mods;
  const names = v.map(b => (b.name || b.id || b.key).toLowerCase());
  assert.deepEqual(names, names.slice().sort((x, y) => x < y ? -1 : x > y ? 1 : 0));
  assert.equal(v.length, 36);
});
test('formatting: sizes, dates, the row line, the count lines', () => {
  assert.equal(modsTab.formatSize(0), '0 B');
  assert.equal(modsTab.formatSize(1536), '1.5 KB');
  assert.equal(modsTab.formatSize(1843230), '1.8 MB');
  assert.equal(modsTab.formatSize(3 * 1073741824), '3.0 GB');
  assert.match(modsTab.formatDate(1793344563), /^2026-10-(29|30) \d\d:\d\d$/);
  assert.equal(modsTab.formatDate(0), '');
  assert.equal(modsTab.subText({id: 'local.hell-guard', version: '0.0.0', size: 1210441, key: 'local.hell-guard'}), 'local.hell-guard \u00b7 0.0.0 \u00b7 1.2 MB');
  assert.equal(modsTab.subText({id: 'female.models', version: '', size: 6275377, key: 'female.models'}), 'female.models \u00b7 6.0 MB');
  assert.equal(modsTab.subText({id: '', key: 'unbuilt-folder'}), 'unbuilt-folder');
  assert.deepEqual(modsTab.countLines({shadows: 500, sounds: 9, movies: 0, modules: 1, vt: 2}),
    ['Replaces 500 stock files', '9 sounds', '1 module', '2 virtual textures']);
  assert.deepEqual(modsTab.countLines(null), []);
  assert.equal(modsTab.filterMods(modsTab.normalize(v3).mods, 'CYBER').length, 2);
});
test('action messages', () => {
  assert.deepEqual(modsTab.ACTIONS, ['install', 'migrate', 'rescan', 'openFolder', 'rebuild', 'verify', 'mapAddMod',
                                     'mapRemoveModPreview', 'mapRemoveMod', 'uninstallPreview', 'uninstall', 'chooseRegistry',
                                     'modFiles', 'modFileText', 'newMod']);
  assert.equal(modsTab.actionMessage('mapUpgradeMod', 'a'), null, 'no build management: the upgrade action is gone');
  assert.deepEqual(modsTab.actionMessage('chooseRegistry', 'entitydef/player/coop/edit/handsModelMPUseDoomHands\tk.a'),
                   {cmd: 'modsAction', action: 'chooseRegistry', arg: 'entitydef/player/coop/edit/handsModelMPUseDoomHands\tk.a'});
  assert.deepEqual(modsTab.actionMessage('rescan'), {cmd: 'modsAction', action: 'rescan'});
  assert.deepEqual(modsTab.actionMessage('uninstall', 'a'), {cmd: 'modsAction', action: 'uninstall', arg: 'a'});
  for (const gone of ['remove', 'palette', 'pruneList', 'pruneApply', 'removedEmpty', 'format-c']) assert.equal(modsTab.actionMessage(gone), null);
});

/* ---- the list --------------------------------------------------------------------------------- */
test('show rescans once; the list waits for the answer', () => {
  const h = harness();
  assert.equal(h.sent.length, 0, 'nothing is requested until the tab is shown');
  h.ui.show();
  h.ui.show();
  assert.deepEqual(h.sent, [{cmd: 'modsAction', action: 'rescan'}], 'a second show while waiting does not ask again');
  assert.ok(!h.sent.some(m => m.cmd === 'modsList'), 'the page asks with a modsAction, the message that arms the host push');
  assert.match(h.nodes.modsList.textContent, /Reading installed mods/);
  answer(h, v3);
  assert.equal(h.toasts.length, 0, 'a successful rescan on show says nothing');
  h.ui.show();
  assert.equal(h.sent.length, 2, 'showing the tab again rereads the folder');
  assert.equal(h.ui.onMessage({kind: 'other'}), false);
});
test('the list: one flat list, one status glyph per row, two lines per row, no pills', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsBadge.textContent, '36');
  assert.equal(h.nodes.modsList.all('mods-group').length + h.nodes.modsList.all('mods-group-head').length +
               h.nodes.modsList.all('pf-caret').length, 0, 'no group headers, no carets');
  assert.ok(h.nodes.modsList.children.every(n => /mods-row/.test(n.className)), 'rows sit directly in the list');
  assert.deepEqual(keys(h), modsTab.normalize(v3).mods.map(b => b.key), 'the model order');
  assert.equal(rows(h).length, 36, 'every row is mounted');
  assert.ok(rows(h).every(n => n.style.top === undefined), 'rows are in flow');
  const hg = rowOf(h, 'local.hell-guard');
  assert.equal(hg.all('mods-row-name')[0].textContent, 'Hell Guards');
  assert.equal(hg.all('mods-row-sub')[0].textContent, 'local.hell-guard \u00b7 0.0.0 \u00b7 1.2 MB');
  assert.ok(!/files|installed game/.test(hg.textContent), 'the file counts left the row');
  /* the open map's mods (the document's map.mods) show the in-this-map glyph, and only those */
  const used = v3.map.mods.map(r => r.key).filter(k => v3.mods.some(b => b.key === k));
  assert.ok(used.length >= 2 && used.indexOf('local.hell-guard') >= 0);
  assert.deepEqual(pills(h).slice().sort(), used.slice().sort());
  assert.ok(rows(h).every(n => n.all('mods-pill').length === 0 && n.all('mods-status-icon').length === 1), 'one glyph, no pill');
  assert.equal(hg.all('mods-status-icon')[0].getAttribute('aria-label'), 'In this map');
  assert.equal(hg.all('mods-status-icon')[0].children[0].children[0].getAttribute('href'), '#icon-map');
  assert.match(hg.title, /The open map uses this mod/);
  assert.equal(hg.getAttribute('data-in-map'), 'true');
  assert.equal(hg.children.length, 2, 'glyph and text, nothing else');
  assert.equal(iconOf(rowOf(h, 'airlock')), 'ok');
  assert.equal(hg.title, 'Installed and serving 24 files, 476 more read from your install. The open map uses this mod.');
  const icon = rowOf(h, 'airlock').all('mods-status-icon')[0];
  assert.equal(icon.children[0].ns, 'http://www.w3.org/2000/svg');
  assert.equal(icon.children[0].children[0].getAttribute('href'), '#icon-check');
  for (const key of ['local.cyberdemon@0.0.0', 'local.cyberdemon@0.0.0-05238b2e']) {
    assert.equal(iconOf(rowOf(h, key)), 'warn', 'needs attention wins over in this map');
    assert.equal(rowOf(h, key).title, modsTab.DUPLICATE_TEXT);
  }
  assert.equal(rowOf(h, 'local.cyberdemon@0.0.0').all('mods-status-icon')[0].children[0].children[0].getAttribute('href'), '#icon-triangle-alert');
  /* a folder another build of its key outranks: installed, with no grey state and no build talk */
  const sup = clone(v3);
  sup.mods.find(b => b.key === 'local.cyberdemon@0.0.0').duplicateId = false;
  sup.mods.find(b => b.key === 'local.cyberdemon@0.0.0').state = 'installed';
  h.ui.onMessage({kind: 'mods', data: sup});
  const grey = rowOf(h, 'local.cyberdemon@0.0.0');
  assert.equal(iconOf(grey), 'ok');
  assert.doesNotMatch(grey.title, /another build/);
  /* the glyph follows the document: no map open, none; a map that drops a mod drops its glyph */
  const closed = clone(v3);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  h.ui.onMessage({kind: 'mods', data: closed});
  assert.deepEqual(pills(h), []);
  const fewer = clone(v3);
  fewer.map.mods = fewer.map.mods.filter(r => r.key === 'local.hell-guard');
  h.ui.onMessage({kind: 'mods', data: fewer});
  assert.deepEqual(pills(h), ['local.hell-guard']);
  rowOf(h, 'local.hell-guard').click();
  assert.equal(iconOf(rowOf(h, 'local.hell-guard')), 'map', 'selecting keeps the glyph');
});
test('the retired collapsed-groups setting is neither read nor written', () => {
  const h = harness();
  shown(h);
  assert.equal(modsTab.COLLAPSE_KEY, undefined);
  assert.equal(h.ui.loadCollapsed, undefined, 'nothing applies a stored value');
  assert.ok(!('collapsed' in h.ui.state()));
  /* a stored value arriving anyway is not the tab's message */
  assert.equal(h.ui.onMessage({kind: 'configValue', key: 'mods.collapsed_groups', result: 1, valueJson: '["yours"]'}), false);
  assert.equal(rows(h).length, 36, 'every row stays shown');
  h.nodes.modsFilter.value = 'hell';
  h.nodes.modsFilter.dispatch('input');
  assert.deepEqual(keys(h), ['local.hell-guard']);
  h.nodes.modsFilter.value = '';
  h.nodes.modsFilter.dispatch('input');
  rowOf(h, 'airlock').click();
  h.nodes.modsList.dispatch('keydown', {key: 'End'});
  assert.equal(h.saved.length, 0, 'nothing is saved');
  assert.ok(!h.sent.some(m => m.cmd === 'configSet' || m.cmd === 'configGet'), 'and nothing is asked of the configuration');
});
test('the filter matches name, id and folder over the flat list', () => {
  const h = harness();
  shown(h);
  h.nodes.modsFilter.value = 'HELL GUARDS';
  h.nodes.modsFilter.dispatch('input');
  assert.deepEqual(keys(h), ['local.hell-guard'], 'by name, without case');
  h.nodes.modsFilter.value = 'mapobjects.';
  h.nodes.modsFilter.dispatch('input');
  assert.deepEqual(keys(h), ['mapobjects.prop'], 'by id (its name is Props)');
  h.nodes.modsFilter.value = '05238b2e';
  h.nodes.modsFilter.dispatch('input');
  assert.deepEqual(keys(h), ['local.cyberdemon@0.0.0-05238b2e'], 'by folder');
  h.nodes.modsFilter.value = 'cyber';
  h.nodes.modsFilter.dispatch('input');
  assert.deepEqual(keys(h), ['local.cyberdemon@0.0.0', 'local.cyberdemon@0.0.0-05238b2e'], 'matches keep the list order');
  h.nodes.modsList.dispatch('keydown', {key: 'Home'});
  assert.equal(h.ui.state().selectedKey, 'local.cyberdemon@0.0.0', 'the keys move over the rows shown');
  h.nodes.modsFilter.value = 'zzz';
  h.nodes.modsFilter.dispatch('input');
  assert.match(h.nodes.modsList.textContent, /No installed mod matches/);
});
test('the attention icon button filters to the rows that need attention; its count is its hover', () => {
  const h = harness();
  assert.equal(h.nodes.modsAttnBtn.style.display, 'none', 'hidden before the first answer');
  shown(h);
  const b = h.nodes.modsAttnBtn;
  assert.equal(b.tagName, 'button', 'a real button: focusable, Enter and Space work');
  assert.equal(b.style.display, '');
  assert.equal(b.textContent, '', 'an icon, no number on the button');
  assert.match(b.title, /^2 mods need attention: show only these$/);
  assert.equal(b.getAttribute('aria-pressed'), 'false');
  assert.equal(b.className, 'btn icon icon-button mods-attn', 'the panel icon button');
  b.click();
  assert.equal(b.getAttribute('aria-pressed'), 'true', 'the pressed state shows');
  assert.match(b.title, /show every mod$/);
  assert.deepEqual(rows(h).map(n => n.getAttribute('data-key')), ['local.cyberdemon@0.0.0', 'local.cyberdemon@0.0.0-05238b2e']);
  b.click();
  assert.equal(rows(h).length, 36);
  /* no attention: the badge hides and the filter lets go */
  b.click();
  const fixed = clone(v3);
  fixed.mods = fixed.mods.filter(m => m.key !== 'local.cyberdemon@0.0.0');
  fixed.mods.find(m => m.id === 'local.cyberdemon').duplicateId = false;
  fixed.mods.find(m => m.id === 'local.cyberdemon').state = 'installed';
  h.ui.onMessage({kind: 'mods', data: fixed});
  assert.equal(b.style.display, 'none');
  assert.equal(h.ui.state().attentionOnly, false);
  assert.equal(rows(h).length, 35);
});
test('keyboard selection over the flat list', () => {
  const h = harness();
  shown(h);
  rowOf(h, 'alex.r2-imp').click();
  assert.equal(h.ui.state().selectedKey, 'alex.r2-imp');
  assert.equal(rowOf(h, 'alex.r2-imp').getAttribute('aria-selected'), 'true');
  let e = h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(e.defaultPrevented, true);
  assert.equal(h.ui.state().selectedKey, 'rctest', 'next row in display order: R2 Imp copy, then rctest');
  assert.ok(rowOf(h, 'rctest').scrolledIntoView > 0);
  h.nodes.modsList.dispatch('keydown', {key: 'End'});
  assert.equal(h.ui.state().selectedKey, 'waterfalls');
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(h.ui.state().selectedKey, 'waterfalls');
  h.nodes.modsList.dispatch('keydown', {key: 'Home'});
  assert.equal(h.ui.state().selectedKey, 'acceptance.mixed-rows');
  e = h.nodes.modsList.dispatch('keydown', {key: 'PageDown'});
  assert.equal(e.defaultPrevented, false, 'Page Down scrolls natively');
});
test('Migrate shows only while overrides holds entries the record does not cover', () => {
  const h = harness();
  assert.equal(h.nodes.modsMigrateBtn.style.display, 'none');
  shown(h);
  assert.equal(h.nodes.modsMigrateBtn.style.display, '');
  assert.match(h.nodes.modsMigrateBtn.title, /2 entries of the old overrides folder/);
  h.nodes.modsMigrateBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'migrate'});
  assert.match(h.nodes.modsStatus.textContent, /Migrating/);
  h.ui.onMessage({kind: 'modsActionResult', action: 'migrate', data: {action: 'migrate', ok: true, message: 'Moved your mods.'}});
  const done = clone(v3);
  done.migrate = {entries: 34, pending: 0, running: false};
  h.ui.onMessage({kind: 'mods', data: done});
  assert.equal(h.nodes.modsMigrateBtn.style.display, 'none');
});

/* ---- the details pane ------------------------------------------------------------------------- */
test('details: every section for a v3 mod', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsUninstallBtn.disabled, true);
  rowOf(h, 'local.hell-guard').click();
  assert.equal(h.nodes.modsUninstallBtn.disabled, false);
  assert.equal(h.nodes.modsRebuildBtn.disabled, false);
  const d = h.nodes.modsDetail;
  assert.equal(d.all('mods-detail-name')[0].textContent, 'Hell Guards');
  assert.equal(d.all('mods-detail-head')[0].all('mods-status-icon')[0].getAttribute('data-status'), 'ok');
  assert.equal(d.all('mods-detail-sentence')[0].textContent, 'Installed and serving 24 files, 476 more read from your install');
  assert.deepEqual(d.all('mods-section').map(n => n.getAttribute('data-section')), ['about', 'adds', 'facts', 'files', 'requirements', 'build', 'sharing']);
  const about = section(h, 'about').textContent;
  assert.ok(!/Build 2/.test(section(h, 'about').textContent), 'no build number');
  for (const s of ['Id local.hell-guard', 'Version 0.0.0', 'Description The three Hell Guard brothers',
                   'Folder local.hell-guard', 'Moved from the old overrides folder (overrides\\hell-guards)'])
    assert.ok(about.indexOf(s) >= 0, s + ' in ' + about);
  assert.equal(section(h, 'about').all('mods-origin')[0].textContent, 'Moved from the old overrides folder (overrides\\hell-guards)',
    'one plain line, not a labelled field');
  assert.ok(!/Origin/.test(about));
  rowOf(h, 'p6probe').click();
  assert.equal(section(h, 'about').all('mods-origin')[0].textContent, 'Imported from an export');
  rowOf(h, 'female.models').click();
  assert.equal(section(h, 'about').all('mods-origin')[0].textContent, 'Downloaded for a map you played');
  rowOf(h, 'alex.r2-imp').click();
  assert.equal(section(h, 'about').all('mods-origin')[0].textContent, 'Added by hand');
  rowOf(h, 'local.hell-guard').click();
  assert.ok(!/Author/.test(about), 'an empty author is left out');
  const adds = section(h, 'adds').textContent;
  assert.match(adds, /3 Toybox rows Hell Guard \(hammer\) Hell Guard \(shield\) Hell Guard \(fist\)/);
  assert.match(adds, /Replaces 498 stock files/);
  assert.match(adds, /9 sounds/);
  assert.ok(!/movie|module|virtual/.test(adds), 'zero counts are left out');
  const files = section(h, 'files');
  assert.match(files.textContent, /Carried 24 files, 1\.2 MB/);
  assert.match(files.textContent, /From the game 476 more read from your install/);
  assert.ok(files.textContent.indexOf(modsTab.FILES_NOTE) >= 0);
  const det = files.tags('details')[0];
  assert.match(det.className, /pc-limits-details/, 'the Limits section\'s details primitive');
  assert.equal(det.tags('summary')[0].textContent, 'Carried paths (24)');
  assert.equal(det.all('mods-paths-list')[0].children.length, 24);
  const req = section(h, 'requirements').textContent;
  assert.match(req, /g_useImageBlackList +0 g_useResourceBlackList +0/);
  assert.ok(req.indexOf(modsTab.NEEDS_NOTE) >= 0);
  const share = section(h, 'sharing').textContent;
  assert.match(share, /Used by 2 of your published maps/);
  const built = section(h, 'build').textContent;
  assert.match(built, /Last built \d{4}-\d\d-\d\d \d\d:\d\d/);
  assert.ok(built.indexOf('Bundle ' + v3.mods.find(m => m.key === 'local.hell-guard').bundle.slice(0, 16)) >= 0);
  assert.match(built, /when you ask for help/);
  /* the folder link opens that folder */
  section(h, 'about').all('mods-link')[0].click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'openFolder', arg: 'local.hell-guard'});
  assert.equal(h.ui.state().busy, '', 'opening a folder does not hold the buttons');
  /* a carried list shorter than the carried count says how many more */
  rowOf(h, 'doors').click();
  assert.match(section(h, 'files').textContent, /and 93 more/);
  /* attention */
  rowOf(h, 'local.cyberdemon@0.0.0-05238b2e').click();
  assert.equal(h.nodes.modsDetail.all('mods-detail-sentence')[0].textContent,
    'Needs attention: ' + modsTab.DUPLICATE_TEXT);
  assert.match(h.nodes.modsDetail.all('mods-detail-sentence')[0].className, /warn/);
  /* Rebuild and Verify carry the key */
  h.nodes.modsRebuildBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'rebuild', arg: 'local.cyberdemon@0.0.0-05238b2e'});
  assert.match(h.nodes.modsStatus.textContent, /Building the mod again/);
  h.ui.onMessage({kind: 'modsActionResult', action: 'rebuild', data: {action: 'rebuild', ok: true, message: 'Built again.'}});
  h.nodes.modsVerifyBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'verify', arg: 'local.cyberdemon@0.0.0-05238b2e'});
});
/* Colour in the details pane: the Decl Text editor's token hues, one kind each, each with a hover title. */
test('details colour: the editor\'s token hues mark own paths, game files, edit counts, settings and Toybox rows', () => {
  assert.deepEqual(modsTab.pathParts('generated/decls/entitydef/mods/bare-hands/sub/x.decl', ['bare-hands']),
    [{text: 'generated/decls/entitydef/', cls: 'mk-dir'}, {text: 'mods/bare-hands/', cls: 'mk-own'},
     {text: 'sub/', cls: 'mk-dir'}, {text: 'x.decl', cls: ''}]);
  assert.deepEqual(modsTab.pathParts('generated/decls/entitydef/player/coop.decl', ['bare-hands']),
    [{text: 'generated/decls/entitydef/player/', cls: 'mk-dir'}, {text: 'coop.decl', cls: 'mk-game'}]);
  assert.equal(modsTab.pathParts('a/mods/other/x.decl', ['bare-hands']).pop().cls, 'mk-game', 'another mod\'s namespace is not this mod\'s');
  assert.equal(modsTab.pathParts('a/mods/bare-hands-2/x.decl', ['bare-hands']).pop().cls, 'mk-game', 'the namespace matches whole');
  assert.deepEqual(modsTab.editParts({set: 4, remove: 6, list: 0, keyed: 0, bag: 0, drop: 2}).map(p => p.kind), ['set', 'removed', 'removed']);
  assert.equal(modsTab.valueClass('1'), 'mk-num');
  assert.equal(modsTab.valueClass('-0.5'), 'mk-num');
  assert.equal(modsTab.valueClass('on'), 'mk-str');
  const doc = clone(v3);
  const hg = doc.mods.find(m => m.key === 'local.hell-guard');
  hg.carriedPaths = ['generated/decls/entitydef/mods/local.hell-guard/hammer.decl',
                     'generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl', 'generated/decls/strings/hellguard.decl'];
  const h = harness();
  shown(h, doc);
  rowOf(h, 'local.hell-guard').click();
  const T = modsTab.PATH_TITLES;
  const carried = section(h, 'files').all('mods-path');
  assert.deepEqual(carried.map(n => n.title), [T.own, T.edit, T.replace]);
  assert.equal(carried[0].all('mk-own')[0].textContent, 'mods/local.hell-guard/');
  assert.equal(carried[0].all('mk-game').length, 0);
  assert.equal(carried[1].all('mk-game')[0].textContent, 'snapmap_default.decl');
  assert.equal(carried[1].all('mk-dir')[0].textContent, 'generated/decls/entitydef/ai/conductor/coop/');
  /* Toybox rows as strings */
  const toybox = section(h, 'adds').all('mods-rows-item');
  assert.equal(toybox.length, 3);
  toybox.forEach(n => { assert.match(n.className, /\bmk-str\b/); assert.equal(n.title, 'a Toybox row this mod adds'); });
  /* the Edits line: the game file's name in the game's hue, what is set and what is removed */
  const facts = section(h, 'facts');
  assert.deepEqual(facts.all('mk-set').map(n => n.textContent), ['3 list entries added', '1 field set']);
  assert.deepEqual(facts.all('mk-removed').map(n => n.textContent), ['1 field removed']);
  assert.deepEqual(facts.all('mods-path').filter(n => n.all('mk-set').length).map(n => n.all('mk-game')[0].textContent),
                   ['snapmap_default', 'hellguard']);
  /* the console settings: the name as a key, the value as a number */
  const req = section(h, 'requirements');
  assert.deepEqual(req.all('mk-key').map(n => n.textContent), ['g_useImageBlackList', 'g_useResourceBlackList']);
  assert.equal(req.all('mk-num').length, 2);
  /* colour is never the only carrier: every coloured part, or the line holding it, has a hover title */
  const titled = n => { for (let p = n; p; p = p.parentNode) if (p.title) return true; return false; };
  ['mk-own', 'mk-game', 'mk-set', 'mk-removed', 'mk-key', 'mk-num', 'mk-str'].forEach(cls =>
    h.nodes.modsDetail.all(cls).forEach(n => assert.ok(titled(n), cls + ' ' + n.textContent + ' has a title')));
  /* the CSS uses the editor's tokens only, and the docs carry the mapping */
  const rules = css.split('\n').filter(l => /\.mods-side \.mk-/.test(l));
  assert.equal(rules.length, 6);
  rules.forEach(l => assert.match(l, /color: var\(--(tkStr|tkBool|tkKey|tkNum|muted)\); \}$/, l));
  assert.ok(docs.indexOf('### Colour in the details pane') >= 0);
  ['mk-own', 'mk-game', 'mk-set', 'mk-removed', 'mk-key', 'mk-num', 'mk-str', 'mk-dir'].forEach(c => assert.ok(docs.indexOf('`' + c + '`') >= 0, c));
});
test('details: requirement commands are listed with the console settings', () => {
  const doc = clone(v3);
  const hg = doc.mods.find(m => m.key === 'local.hell-guard');
  hg.requirements = {commands: {activate: ['bind x "say hi"', 'echo on'], deactivate: ['echo off']},
                     cvars: {com_maxFps: '120'}};
  const m = modsTab.normalize(doc).mods.find(b => b.key === 'local.hell-guard');
  assert.deepEqual(m.commands, {activate: ['bind x "say hi"', 'echo on'], deactivate: ['echo off']});
  assert.deepEqual(modsTab.normalize(v3).mods.find(b => b.key === 'local.hell-guard').commands, {activate: [], deactivate: []});
  assert.equal(modsTab.normalize(v2).mods.find(b => b.key === 'alex.r2-imp@1.0.0+2').commands, null);
  const h = harness();
  shown(h, doc);
  rowOf(h, 'local.hell-guard').click();
  const req = section(h, 'requirements').textContent;
  assert.match(req, /Console settings com_maxFps +120/);
  assert.match(req, /Commands when active bind x "say hi" echo on/);
  assert.match(req, /Commands when removed echo off/);
  rowOf(h, 'doors').click();
  assert.ok(!/Commands when/.test(section(h, 'requirements').textContent), 'no commands, no rows');
});
test('details: a v2 document renders with the v3 parts left out', () => {
  const h = harness();
  shown(h, v2);
  rowOf(h, 'alex.r2-imp@1.0.0+2').click();
  const d = h.nodes.modsDetail;
  assert.deepEqual(d.all('mods-section').map(n => n.getAttribute('data-section')), ['about', 'adds', 'files', 'requirements']);
  assert.equal(section(h, 'about').all('mods-origin')[0].textContent, 'Added by hand');
  assert.match(section(h, 'adds').textContent, /^What it adds 2 Toybox rows$/);
  assert.equal(section(h, 'files').tags('details').length, 0, 'no carried list without carriedPaths');
  assert.match(section(h, 'files').textContent, /Carried 13 files, 1\.8 MB/);
  assert.equal(section(h, 'requirements').textContent, 'Requirements ' + modsTab.NEEDS_NOTE);
  rowOf(h, 'unbuilt-folder').click();
  assert.match(d.textContent, /Needs attention: not built yet: bundle\.json is missing/);
  assert.match(section(h, 'about').textContent, /Id \(not built yet\)/);
  assert.match(section(h, 'adds').textContent, /Adds no Toybox rows/);
  assert.equal(rowOf(h, 'unbuilt-folder').all('mods-row-sub')[0].textContent, 'unbuilt-folder');
  /* v2 has no origins and no map mods: every row is a plain row without a pill */
  assert.deepEqual(pills(h), []);
  assert.match(h.nodes.modsAttnBtn.title, /^4 mods need attention/);
});
test('builder warnings: one plain line each under Build; a warning is not attention', () => {
  const doc = clone(v3);
  const air = doc.mods.find(m => m.key === 'airlock');
  air.warnings = ['mod.json "displayName" is not published to the store; use "name"', 'assets/x.decl names y, which no archive holds'];
  air.firstWarning = air.warnings[0];
  const h = live(doc);
  h.ui.select('airlock');
  const lines = section(h, 'build').all('mods-warning').map(n => n.textContent);
  assert.deepEqual(lines, air.warnings);
  assert.equal(iconOf(rowOf(h, 'airlock')), 'ok', 'installed, not needs attention');
  assert.equal(modsTab.statusInfo(modsTab.normalize(doc).mods.find(m => m.key === 'airlock')).tone, 'ok');
  /* only firstWarning: that one line; neither: no line */
  delete air.warnings;
  assert.deepEqual(modsTab.normalize(doc).mods.find(m => m.key === 'airlock').warnings, [air.firstWarning]);
  delete air.firstWarning;
  assert.deepEqual(modsTab.normalize(doc).mods.find(m => m.key === 'airlock').warnings, []);
  /* the stand-in host carries the warning through an action's fresh document */
  const doc2 = clone(v3);
  doc2.mods.find(m => m.key === 'airlock').firstWarning = 'assets/x.decl names y, which no archive holds';
  const host = modsTab.standIn(doc2);
  const out = host({cmd: 'modsAction', action: 'rebuild', arg: 'airlock'});
  const h2 = live(out[1].data);
  h2.ui.select('airlock');
  assert.deepEqual(section(h2, 'build').all('mods-warning').map(n => n.textContent), ['assets/x.decl names y, which no archive holds']);
  assert.equal(iconOf(rowOf(h2, 'airlock')), 'ok');
});
test('empty states: How mods work, with and without mods', () => {
  const h = harness();
  shown(h);
  assert.match(h.nodes.modsDetail.textContent, /^How mods work A mod is a folder in mods\\ holding mod\.json and an assets\\ folder\. Maps record which mods they use\. Players who load one are offered the download\. Select a mod/);
  const empty = clone(v3);
  empty.mods = []; empty.migrate = {entries: 0, pending: 0, running: false};
  h.ui.onMessage({kind: 'mods', data: empty});
  assert.match(h.nodes.modsList.textContent, /No mods installed\. Drop a mod folder into mods\\ or use the download button/);
  assert.match(h.nodes.modsDetail.textContent, /How mods work .* drop a folder into mods\\ or use the download button\./);
  assert.equal(h.nodes.modsBadge.textContent, '0');
  assert.equal(h.nodes.modsAttnBtn.style.display, 'none');
  h.ui.onMessage({kind: 'mods', data: null, error: 'The game did not answer in time.'});
  assert.match(h.nodes.modsList.textContent, /did not answer in time/);
  assert.equal(h.nodes.modsDetail.children.length, 0);
  h.ui.onMessage({kind: 'mods', data: Object.assign(clone(v3), {storeReadable: false})});
  assert.match(h.nodes.modsList.textContent, /cannot be read/);
  const lone = harness();
  lone.ui.show();
  lone.ui.onMessage({kind: 'modsActionResult', action: 'rescan', ok: false, message: "This build's backend has no Mods tab.", data: null});
  assert.match(lone.nodes.modsList.textContent, /no Mods tab/);
  assert.equal(lone.ui.state().waiting, false);
});
test('toolbar actions post their messages and hold the buttons while busy', () => {
  const h = harness();
  shown(h);
  h.nodes.modsInstallBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'install'}, 'the host opens the folder picker; the page names no path');
  assert.equal(h.nodes.modsRefreshBtn.disabled, true);
  assert.equal(h.ui.act('migrate'), false);
  assert.equal(h.toasts.pop().kind, 'warn');
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', data: {action: 'install', ok: true, message: 'Installed kim.new-crates.', key: 'kim.new-crates'}});
  assert.equal(h.ui.state().selectedKey, 'kim.new-crates');
  h.nodes.modsInstallBtn.click();
  const before = h.toasts.length;
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', ok: false, message: '', data: {cancelled: true}});
  assert.equal(h.toasts.length, before);
  h.nodes.modsRefreshBtn.click();
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', ok: false, message: 'The game did not run the request in time.', data: null});
  assert.equal(h.nodes.modsStatus.className, 'mods-status err');
  h.nodes.modsFolderBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'openFolder'});
});

/* ---- the map modal ---------------------------------------------------------------------------- */
test('the map modal opens and closes like the decl focus modal', () => {
  const h = live();
  const ov = h.nodes.modsMapModal;
  assert.equal(ov.style.display, 'none');
  h.nodes.modsMapBtn.click();
  assert.equal(ov.style.display, 'flex');
  assert.equal(ov.getAttribute('aria-hidden'), 'false');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Hell Guard Arena');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].id, 'modsMapTitle');
  assert.match(activeElement.className, /mods-map-close/, 'focus moves into the dialog');
  const t = modalText(h);
  for (const s of ['Name Hell Guard Arena', 'Saved map e2c1f0a9b7d34c58', 'Published Yes, 2026-10-2', 'Mod store Its mods are uploaded',
                   'Search tag mods (present)', 'Mods 5 mods, 13 MB for a player to download', 'Last saved 2026-10-'])
    assert.ok(t.indexOf(s) >= 0, s);
  const mr = h.nodes.modsMapHost.all('mods-map-row');
  assert.equal(mr.length, 5);
  assert.match(mr[0].textContent, /^Hell Guards local\.hell-guard \u00b7 0\.0\.0 \u00b7 1\.2 MB 12 placed entities use it Also used by timeline: Wave 2: spawn the guards, wire: Relay: all guards dead Remove from map$/);
  assert.equal(iconOf(mr[4]), 'warn');
  assert.match(mr[4].textContent, /jo\.lava-floor .* 2 placed entities use it; not installed on this computer; playing the map downloads it/);
  /* Escape, the backdrop and the close button each close it; focus returns to the opener */
  let e = ov.dispatch('keydown', {key: 'Escape'});
  assert.equal(e.defaultPrevented, true);
  assert.equal(ov.style.display, 'none');
  assert.equal(ov.getAttribute('aria-hidden'), 'true');
  assert.equal(activeElement, h.nodes.modsMapBtn);
  h.nodes.modsMapBtn.click();
  ov.dispatch('mousedown', {target: h.nodes.modsMapHost});
  assert.equal(ov.style.display, 'flex', 'a press inside the dialog does not close it');
  ov.dispatch('mousedown', {target: ov});
  assert.equal(ov.style.display, 'none');
  h.nodes.modsMapBtn.click();
  h.nodes.modsMapHost.all('mods-map-close')[0].click();
  assert.equal(ov.style.display, 'none');
  /* no map open, and a v2 map */
  const closed = clone(v3);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  const c = live(closed);
  c.nodes.modsMapBtn.click();
  assert.match(modalText(c), /This map Open a map in the editor to see the mods it uses\./);
  const old = live(v2);
  old.nodes.modsMapBtn.click();
  assert.equal(old.nodes.modsMapHost.all('mods-section').length, 1, 'v2: details only, nothing to list');
});
test('Remove from map: the dry run drives the confirmation; two buttons; the document drives the result', () => {
  const h = live();
  h.nodes.modsMapBtn.click();
  const removeBtn = () => h.nodes.modsMapHost.all('mods-map-row')[0].tags('button').find(b => b.textContent === 'Remove from map');
  removeBtn().click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveModPreview', arg: 'local.hell-guard'});
  assert.equal(h.ui.state().modal.page, 'confirm');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Remove Hell Guards from Hell Guard Arena?');
  const items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.match(items[0], /^Deletes 12 placed entities from Hell Guard Arena\. Show all 12 Hell Guard \(hammer\) 1/);
  assert.equal(h.nodes.modsMapHost.tags('li')[0].tags('details').length, 1, 'a long name list is collapsible');
  assert.match(items[1], /^Severs 2 wire or timeline references to those entities\. timeline: Wave 2: spawn the guards wire: Relay: all guards dead$/);
  assert.ok(!items.some(t => /Toybox/.test(t)), 'Toybox rows are not a consequence of removing a mod from a map');
  assert.equal(items[2], "Removes the mod's row from the map's list of mods.");
  assert.equal(items[3], 'The published copy on the server is unaffected until you publish the map again.');
  assert.equal(items.length, 4, 'not the last mod: the tag stays');
  const btns = modalButtons(h);
  assert.deepEqual(btns.map(b => b.textContent), ['Cancel', 'Delete from map'], 'exactly two buttons; the label carries the scope');
  const notes = h.nodes.modsMapHost.all('mods-note').map(n => n.textContent);
  assert.deepEqual(notes.slice(0, 2), [modsTab.SCOPE.mapRemoveMod, 'Ctrl+Z in the editor brings the entities back, also after a Play round. Removing it:'], 'the scope sentence comes first');
  assert.equal(modsTab.SCOPE.mapRemoveMod, 'Only this map changes. The mod stays installed on this computer and keeps its Toybox rows.');
  /* the buttons are a footer: the dialog's last child, after the scrolling consequence list */
  const hostKids = h.nodes.modsMapHost.children;
  assert.match(hostKids[hostKids.length - 1].className, /^modal-actions mods-map-foot$/);
  assert.match(hostKids[hostKids.length - 2].className, /^mods-map-body$/);
  assert.equal(hostKids[hostKids.length - 2].all('mods-consequences').length, 1);
  assert.match(btns[1].className, /danger-solid/);
  btns[0].click();
  assert.equal(h.ui.state().modal.page, 'list', 'Cancel goes back to the list');
  assert.equal(h.nodes.modsMapHost.all('mods-map-row').length, 5);
  removeBtn().click();
  modalButtons(h)[1].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveMod', arg: 'local.hell-guard'});
  assert.equal(h.ui.state().modal.page, 'list');
  assert.equal(h.nodes.modsMapHost.all('mods-map-row').length, 4);
  assert.match(modalText(h), /Mods 4 mods/);
  assert.match(h.toasts.pop().text, /Removed Hell Guards from Hell Guard Arena: 12 entities deleted\./);
  /* the page renders from documents only: a result alone changes nothing on screen */
  const hand = harness();
  shown(hand);
  hand.nodes.modsMapBtn.click();
  hand.nodes.modsMapHost.all('mods-map-row')[1].tags('button').find(b => b.textContent === 'Remove from map').click();
  hand.ui.onMessage({kind: 'modsActionResult', action: 'mapRemoveModPreview', ok: true, data: {ok: true, preview: {action: 'mapRemoveMod', key: 'doors', name: 'All doors',
    map: {name: 'Hell Guard Arena', entities: ['Blast door 1'], entityCount: 1, references: [], manifestRow: true}}}});
  assert.deepEqual(hand.nodes.modsMapHost.tags('li').map(n => n.textContent), ['Deletes 1 placed entity from Hell Guard Arena. Blast door 1',
    "Removes the mod's row from the map's list of mods."], 'only what the dry run says');
  modalButtons(hand)[1].click();
  hand.ui.onMessage({kind: 'modsActionResult', action: 'mapRemoveMod', ok: true, data: {ok: true, message: 'Removed.'}});
  assert.equal(hand.nodes.modsMapHost.all('mods-map-row').length, 5, 'still the last document');
  /* a refused dry run says why and stays on the list */
  hand.nodes.modsMapHost.all('mods-map-row')[1].tags('button').find(b => b.textContent === 'Remove from map').click();
  hand.ui.onMessage({kind: 'modsActionResult', action: 'mapRemoveModPreview', ok: false, message: 'The editor is busy.', data: null});
  assert.deepEqual(hand.toasts.pop(), {text: 'The editor is busy.', kind: 'err'});
  assert.equal(hand.ui.state().modal.page, 'list');
});
test('the last removal leaves a vanilla map', () => {
  const h = live();
  h.nodes.modsMapBtn.click();
  for (let n = 5; n > 1; n--) {
    h.nodes.modsMapHost.all('mods-map-row')[0].tags('button').find(b => b.textContent === 'Remove from map').click();
    modalButtons(h)[1].click();
  }
  h.nodes.modsMapHost.all('mods-map-row')[0].tags('button').find(b => b.textContent === 'Remove from map').click();
  const items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.ok(items.indexOf('Removes the search tag mods: this is the last mod the map uses.') >= 0);
  assert.ok(items.indexOf('The map then needs no mods and plays without Snapmap+.') >= 0);
  modalButtons(h)[1].click();
  assert.match(modalText(h), /This map is now vanilla: it uses no mods and plays without Snapmap\+\./);
  assert.match(modalText(h), /Search tag mods \(absent\)/);
  assert.match(modalText(h), /Mods 0 mods(?! ,)/);
});
test('Uninstall: with the open map using the mod, and without', () => {
  const h = live();
  rowOf(h, 'doors').click();
  h.nodes.modsUninstallBtn.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'uninstallPreview', arg: 'doors'});
  assert.equal(h.nodes.modsMapModal.style.display, 'flex', 'the confirmation opens in the modal');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Uninstall All doors?');
  let items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.match(items[0], /^Deletes 5 placed entities from Hell Guard Arena\. Blast door 1 Blast door 2 Blast door 3 Lift door 1 Lift door 2$/);
  assert.equal(items[1], 'Severs 1 wire or timeline reference to those entities. wire: Arena gate: open on wave 3');
  assert.ok(items.indexOf('Removes 4 Toybox rows from the editor (Lift door, Blast door, Hell gate, Vent door).') >= 0);
  assert.ok(items.indexOf("Removes the mod's row from the map's list of mods.") >= 0);
  assert.equal(items[items.length - 1], 'Moves the folder mods\\doors to the Windows Recycle Bin.', 'Uninstall recycles the folder');
  assert.ok(!items.some(t => /cannot be undone|Undo history/.test(t)), 'the undo line is the head, not a consequence');
  assert.deepEqual(modalButtons(h).map(b => b.textContent), ['Cancel', 'Uninstall']);
  assert.equal(h.nodes.modsMapHost.all('mods-scope')[0].textContent, 'Removes the mod from this computer. The open map is cleaned of it first.');
  assert.equal(h.nodes.modsMapHost.all('mods-note')[1].textContent, modsTab.UNINSTALL_HEAD.undo);
  assert.equal(modsTab.UNINSTALL_HEAD.undo, 'Undo history is cleared. The folder can be restored from the Recycle Bin by hand. Uninstalling:');
  assert.equal(modsTab.UNINSTALL_HEAD.plain, 'The folder can be restored from the Recycle Bin by hand. Uninstalling:');
  assert.match(modalButtons(h)[1].className, /danger-solid/);
  modalButtons(h)[0].click();
  assert.equal(h.nodes.modsMapModal.style.display, 'none', 'Cancel from the details pane closes the dialog');
  assert.equal(activeElement, h.nodes.modsUninstallBtn);
  h.nodes.modsUninstallBtn.click();
  modalButtons(h)[1].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'uninstall', arg: 'doors'});
  assert.equal(h.nodes.modsMapModal.style.display, 'none');
  assert.equal(rowOf(h, 'doors'), undefined);
  assert.equal(h.nodes.modsBadge.textContent, '35');
  assert.equal(h.ui.state().selectedKey, '');
  assert.ok(!h.ui.state().model.map.mods.some(r => r.key === 'doors'), 'the open map was sanitised');
  /* not used by the open map: the author's own folder is deleted too */
  rowOf(h, 'alex.mp-basin').click();
  h.nodes.modsUninstallBtn.click();
  items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.deepEqual(items, ['Removes 2 Toybox rows from the editor (Basin floor, Basin rim).',
                           'Moves the folder mods\\alex.mp-basin to the Windows Recycle Bin.']);
  assert.equal(h.nodes.modsMapHost.all('mods-scope')[0].textContent, 'Removes the mod from this computer. The open map does not use it.');
  assert.equal(h.nodes.modsMapHost.all('mods-note')[1].textContent, modsTab.UNINSTALL_HEAD.undo, 'a map is open: its history is cleared');
  modalButtons(h)[1].click();
  assert.equal(rowOf(h, 'alex.mp-basin'), undefined);
  /* uninstalling one of two duplicates clears the other's attention */
  rowOf(h, 'local.cyberdemon@0.0.0').click();
  h.nodes.modsUninstallBtn.click();
  modalButtons(h)[1].click();
  assert.equal(iconOf(rowOf(h, 'local.cyberdemon@0.0.0-05238b2e')), 'ok');
  assert.equal(h.nodes.modsAttnBtn.style.display, 'none');
});
test('Uninstall goes to the Recycle Bin: saved maps that use it, the undo history, no bin of its own', () => {
  /* the dry run's savedMaps and undoCleared become the confirmation's lines */
  const pv = modsTab.normPreview({action: 'uninstall', install: {folder: 'hell-guards', disposition: 'move', savedMaps: 3, undoCleared: true}});
  assert.deepEqual(pv.install, {folder: 'hell-guards', disposition: 'recycle', savedMaps: 3, undoCleared: true, keptBuild: ''});
  assert.deepEqual(modsTab.consequenceLines(pv).map(c => c.text), [
    'Moves the folder mods\\hell-guards to the Windows Recycle Bin.',
    '3 of your saved maps use it; they will not open until a mod folder provides its assets again.']);
  assert.equal(modsTab.uninstallHead(pv), modsTab.UNINSTALL_HEAD.undo);
  /* none of them, or the saves could not be read: no line; no map open: the history is left alone */
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall', install: {folder: 'a', savedMaps: 0}})).map(c => c.text),
    ['Moves the folder mods\\a to the Windows Recycle Bin.']);
  /* another folder of the key stays: the maps open with it, nothing says they will not open */
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall',
    install: {folder: 'hell-guards@1a2b', savedMaps: 0, keptBuild: 'hell-guards'}})).map(c => c.text),
    ['Moves the folder mods\\hell-guards@1a2b to the Windows Recycle Bin.',
     'Another folder of this mod stays installed (mods\\hell-guards); maps that use the mod open with it.']);
  assert.equal(modsTab.uninstallHead(modsTab.normPreview({action: 'uninstall', install: {folder: 'a'}})), modsTab.UNINSTALL_HEAD.plain,
    'no map open: the head says nothing about undo');
  assert.equal(modsTab.normPreview({action: 'uninstall', install: {folder: 'a'}}).install.savedMaps, -1);
  /* an uninstall's result says what happened; the footer has no bin line */
  const h = live();
  rowOf(h, 'alex.mp-basin').click();
  h.nodes.modsUninstallBtn.click();
  modalButtons(h)[1].click();
  assert.match(h.toasts.slice(-1)[0].text, /^Uninstalled .*\. Its folder was moved to the Recycle Bin and the undo history was cleared\.$/);
  assert.ok(!('removed' in h.ui.state().model), 'a document\'s removed object is not read');
  /* a map row whose folder lacks a listed name */
  assert.deepEqual(modsTab.mapModStatus({state: 'incomplete'}).cause, 'installed, but it lacks files this map uses');
});
test('the stand-in host answers as the host does', () => {
  const store = modsTab.standIn(v3);
  const r = store({cmd: 'modsAction', action: 'rescan'});
  assert.deepEqual(r.map(m => m.kind), ['modsActionResult', 'mods']);
  assert.notEqual(r[1].data, v3, 'a copy, never the original');
  assert.deepEqual(store({cmd: 'modsAction', action: 'openFolder'}), []);
  assert.equal(store({cmd: 'modsAction', action: 'install'})[0].ok, false);
  assert.equal(store({cmd: 'modsAction', action: 'uninstallPreview', arg: 'nope'})[0].ok, false);
  assert.ok(!('editorRows' in store({cmd: 'modsAction', action: 'mapRemoveModPreview', arg: 'doors'})[0].data.preview));
  assert.deepEqual(store({cmd: 'modsAction', action: 'uninstallPreview', arg: 'local.hell-guard'})[0].data.preview.editorRows,
    ['Hell Guard (hammer)', 'Hell Guard (shield)', 'Hell Guard (fist)']);
  assert.equal(v3.mods.length, 36, 'the fixture is untouched');
});

/* ---- the page and host contract --------------------------------------------------------------- */
test('Use in this map: a map-level mod is listed, marked, and removed again from the details pane', () => {
  const h = live();
  const use = h.nodes.modsUseBtn;
  assert.equal(use.style.display, '', 'shown while a map is open');
  assert.ok(use.disabled, 'nothing selected');
  h.ui.select('airlock');
  assert.equal(use.textContent, 'Use in this map');
  assert.ok(!use.disabled && !/danger/.test(use.className));
  use.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapAddMod', arg: 'airlock'});
  assert.match(h.toasts.pop().text, /^Airlock is now used by the whole of Hell Guard Arena\./);
  assert.equal(use.textContent, 'Remove from map', 'the document now lists it');
  assert.match(use.className, /danger/);
  h.nodes.modsMapBtn.click();
  const row = h.nodes.modsMapHost.all('mods-map-row').find(n => n.getAttribute('data-key') === 'airlock');
  assert.ok(row && row.getAttribute('data-attached') === 'true');
  assert.equal(row.all('mods-map-level')[0].textContent, modsTab.MAP_LEVEL_TEXT);
  h.ui.closeMap();
  use.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveModPreview', arg: 'airlock'});
  assert.equal(h.ui.state().modal.page, 'confirm');
  const items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.equal(items[0], "Removes the mod from the map's list of mods; the whole map used it.", 'nothing is deleted');
  modalButtons(h)[1].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveMod', arg: 'airlock'});
  assert.equal(h.ui.state().modal.open, false, 'started from the details pane: the dialog closes');
  assert.equal(use.textContent, 'Use in this map');
  /* already attached: the stand-in says so; no map open: hidden */
  const h2 = live();
  h2.ui.select('airlock');
  h2.nodes.modsUseBtn.click();
  h2.ui.act('mapAddMod', 'airlock');
  assert.match(h2.toasts.pop().text, /already used by the whole of Hell Guard Arena/);
  const closed = clone(v3);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  const h3 = live(closed);
  assert.equal(h3.nodes.modsUseBtn.style.display, 'none');
});
test('scope: About has no Applies line; a mod applies in the maps that declare it (DESIGN-additive 2.3)', () => {
  /* The backend's scope is "always" exactly when the bundle carries editor tweaks, which a mixed
     folder's additions do not share; where tweaks apply is the Editor tweaks line's to say. */
  const doc = clone(v3);
  const by = k => doc.mods.find(m => m.key === k);
  by('airlock').scope = 'map'; by('airlock').scoped = true;
  by('local.my-overrides').scope = 'always'; by('local.my-overrides').scopeAuto = true;
  const h = live(doc);
  for (const key of ['airlock', 'local.my-overrides', 'dlc']) {
    h.ui.select(key);
    assert.ok(!/Applies|In every map|Only in maps that use it/.test(section(h, 'about').textContent), key);
  }
  assert.equal(modsTab.scopeText, undefined);
});
/* ---- the additive model (DESIGN-additive 2.1 to 2.9) ------------------------------------------- */
test('additive: derived facts, the game files a mod edits, encounters; no legacy state', () => {
  const n = modsTab.normalize(v3), m = k => n.mods.find(x => x.key === k);
  const r2 = m('alex.r2-imp');
  assert.equal(r2.facts.adds, 13);
  assert.deepEqual(r2.facts.edits.map(e => e.file), ['generated/decls/entitydef/player/coop.decl', 'generated/decls/aifsmmanager/states/imp.decl']);
  const lines = modsTab.factLines(r2);
  assert.deepEqual(lines.map(l => l.label), ['Adds', 'Edits', 'Encounters']);
  assert.match(lines[1].text, /^edits 2 game files; maps that declare it get the game's text with these edits$/);
  assert.deepEqual(lines[1].list, ['entitydef/player/coop: 3 fields set', 'aifsmmanager/states/imp: 1 named entry added or replaced']);
  assert.deepEqual(lines[2].list, ['encounter mods/alex.r2-imp/single_imp']);
  const mo = modsTab.factLines(m('local.my-overrides'));
  assert.equal(mo[0].label, 'Editor tweaks');
  assert.equal(mo[0].text, '63 files, on this computer only: ' + modsTab.EDITOR_TWEAKS_TEXT);
  assert.match(mo[0].text, /apply in your editor whatever map is open/, 'they always apply in this editor (DESIGN-additive 2.6)');
  assert.ok(!/never in a map/.test(mo[0].text), 'not "never in a map": they apply in every map this editor opens');
  const sp = modsTab.factLines(m('local.spider-mastermind')).find(l => l.label === 'Edits');
  assert.deepEqual(sp.list, ['aifsmmanager/states/spidermastermind: 2 named entries added or replaced']);
  const hg = modsTab.factLines(m('local.hell-guard'));
  assert.deepEqual(hg.find(l => l.label === 'Edits').list,
                   ['entitydef/ai/conductor/coop/snapmap_default: 3 list entries added', 'aifsmmanager/states/hellguard: 1 field set, 1 field removed']);
  assert.match(hg.find(l => l.label === 'Replaces').text, /^498 game files \(a folder the product received\)$/);
  assert.equal(m('local.hell-guard').legacy, undefined, 'a converted package is a mod like any other');
  assert.equal(modsTab.normalize(v2).mods[0].facts, null, 'a v2 document sends no facts');
  const h = live();
  h.ui.select('local.hell-guard');
  const fs = section(h, 'facts');
  assert.ok(fs && /Edits: edits 2 game files/.test(fs.textContent));
  assert.doesNotMatch(h.nodes.modsDetail.textContent, /legacy|grandfather|first-generation|rebuild to isolate/i);
  assert.ok(!/legacy|grandfather/i.test(source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '')),
    'no legacy vocabulary in the page outside comments');
  h.ui.select('alex.r2-imp');
  assert.match(section(h, 'facts').textContent, /Edits: edits 2 game files/);
  assert.ok(!/Registries|player\.|extends/.test(section(h, 'facts').textContent), 'no intent keys shown');
  /* Editor tweaks are counted as editor tweaks, never as Toybox rows the mod adds. */
  h.ui.select('local.my-overrides');
  assert.match(section(h, 'adds').textContent, /^What it adds 63 editor tweaks/);
  assert.ok(!/Toybox row/.test(section(h, 'adds').textContent), 'an editor tweak is not a Toybox row');
  h.ui.select('airlock');
  assert.equal(section(h, 'facts'), undefined, 'nothing derived: no section');
});
test('additive: an edited copy is an edit, never a replacement (facts, counts, the details pane)', () => {
  const doc = clone(v3);
  const r2 = doc.mods.find(m => m.key === 'alex.r2-imp');
  /* as the backend writes a mod whose one game file is an edited copy: classes.edits, not replaces */
  r2.counts.shadows = 0;
  r2.facts.replaces = 0;
  r2.facts.replacements = [{path: 'generated/decls/entitydef/player/coop.decl', edits: true}];
  const m = modsTab.normalize(doc).mods.find(x => x.key === 'alex.r2-imp');
  const lines = modsTab.factLines(m);
  assert.deepEqual(lines.map(l => l.label), ['Adds', 'Edits', 'Encounters']);
  assert.ok(!lines.some(l => l.label === 'Replaces' || /\breplaces\b/i.test(l.text)), 'no line says it replaces');
  assert.deepEqual(modsTab.countLines(m.counts).filter(t => /Replaces/.test(t)), []);
  const h = live(doc);
  h.ui.select('alex.r2-imp');
  assert.ok(!/Replaces/.test(section(h, 'facts').textContent) && !/Replaces/.test(section(h, 'adds').textContent));
  /* a received folder's replacement still reads Replaces */
  r2.facts.replaces = 1;
  r2.facts.replacements.push({path: 'generated/decls/entitydef/monster/imp.decl', edits: false});
  const again = modsTab.factLines(modsTab.normalize(doc).mods.find(x => x.key === 'alex.r2-imp')).find(l => l.label === 'Replaces');
  assert.equal(again.text, '1 game file (a folder the product received)');
  assert.deepEqual(again.list, ['generated/decls/entitydef/monster/imp.decl']);
});
test('additive: a pushed document (a console verb) refreshes the open map modal and keeps the status line', () => {
  const doc = clone(v3);
  const h = live(doc);
  h.ui.openMap();
  assert.equal(h.nodes.modsMapHost.tags('select').length, 0);
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', data: {action: 'rescan', ok: true, message: 'Mods folder reread.'}});
  const status = h.nodes.modsStatus.textContent;
  /* `sh_mods map add` attached a mod: the clash it brings now has two providers */
  const next = clone(v3);
  next.gen = 13;
  next.map.registryClashes = [{slot: 'entitydef/player/coop/edit/handsModelMPUseDoomHands',
                               providers: [{id: 'bare-hands', name: 'Bare Hands'}, {id: 'v6-clash', name: 'V6 clash'}],
                               chosen: '', serving: 'bare-hands'}];
  h.ui.onMessage({kind: 'mods', pushed: true, data: next});
  const sel = h.nodes.modsMapHost.tags('select');
  assert.equal(sel.length, 1, 'the open modal shows the pushed document');
  assert.equal(sel[0].getAttribute('data-slot'), 'entitydef/player/coop/edit/handsModelMPUseDoomHands');
  assert.equal(h.nodes.modsStatus.textContent, status, 'a pushed document keeps the status line');
  /* `sh_mods uninstall` removed a folder: the list follows */
  const fewer = clone(next);
  fewer.mods = fewer.mods.filter(m => m.key !== 'airlock');
  h.ui.onMessage({kind: 'mods', pushed: true, data: fewer});
  assert.ok(!keys(h).includes('airlock'));
});
test('additive: a row at another build still offers the clash choice its installed build makes', () => {
  /* the backend resolves a row pinning a build that is not installed to the build that serves it
     (modui.c mu_row_serving_entry), so the clash reaches the page with both providers */
  const doc = clone(v3);
  doc.map.mods.find(x => x.id === 'jo.lava-floor').state = 'otherBuild';
  doc.map.registryClashes = [{slot: 'entitydef/monster/imp/size', providers: [{id: 'alex.edits', name: 'Edits'}, {id: 'alex.edits2', name: 'Edits Two'}],
                              chosen: '', serving: 'alex.edits'}];
  const h = live(doc);
  h.ui.openMap();
  assert.match(modalText(h), /not installed on this computer; playing the map downloads it/);
  assert.doesNotMatch(modalText(h), /another build/);
  const sel = h.nodes.modsMapHost.tags('select');
  assert.equal(sel.length, 1);
  assert.match(modalText(h), /entitydef\/monster\/imp\/size Edits \(alex\.edits\) Edits Two \(alex\.edits2\)/);
  assert.match(modalText(h), /applies now and is saved with the map's next save/);
});
test('additive: stock banks a mod\'s borrowed sound events need, with this session\'s state', () => {
  const doc = clone(v3);
  const sp = doc.mods.find(m => m.key === 'local.spider-mastermind');
  sp.facts.registries = Object.assign({}, sp.facts.registries, {banks: ['doom_weapon_sp.bnk']});
  sp.facts.stockBanks = [{bank: 'doom_weapon_sp.bnk', state: 'loaded', ms: 201, result: 1}];
  const n = modsTab.normalize(doc), m = n.mods.find(x => x.key === 'local.spider-mastermind');
  assert.deepEqual(m.facts.stockBanks, [{bank: 'doom_weapon_sp.bnk', state: 'loaded', ms: 201, result: 1}]);
  const line = modsTab.factLines(m).find(l => l.label === 'Game sounds');
  assert.match(line.text, /^needs doom_weapon_sp\.bnk, single-player sounds SnapMap skips/);
  assert.deepEqual(line.list, ['doom_weapon_sp.bnk: loaded (201 ms)']);
  /* registries without a state: not loaded yet; a failure names its result */
  delete sp.facts.stockBanks;
  assert.deepEqual(modsTab.factLines(modsTab.normalize(doc).mods.find(x => x.key === 'local.spider-mastermind'))
    .find(l => l.label === 'Game sounds').list, ['doom_weapon_sp.bnk: not loaded']);
  sp.facts.stockBanks = [{bank: 'doom_weapon_sp.bnk', state: 'failed', ms: 0, result: 69}];
  assert.deepEqual(modsTab.factLines(modsTab.normalize(doc).mods.find(x => x.key === 'local.spider-mastermind'))
    .find(l => l.label === 'Game sounds').list, ['doom_weapon_sp.bnk: did not load (AKRESULT 69)']);
  /* nothing needed: no line */
  assert.equal(modsTab.factLines(modsTab.normalize(v3).mods.find(x => x.key === 'local.spider-mastermind'))
    .find(l => l.label === 'Game sounds'), undefined);
  const h = live(doc);
  h.ui.select('local.spider-mastermind');
  assert.match(section(h, 'facts').textContent, /Game sounds: needs doom_weapon_sp\.bnk/);
});
test('additive: no list of builds per key; a failed build', () => {
  const h = live();
  h.ui.select('local.cyberdemon@0.0.0');
  assert.equal(section(h, 'builds'), undefined, 'no builds section: the player manages no builds');
  assert.doesNotMatch(h.nodes.modsDetail.textContent, /Build \d|newest/);
  const doc = clone(v3);
  doc.mods.find(x => x.key === 'airlock').buildFailed = {cause: 'assets/x.decl: parse error', snapshot: true, servingBuild: 1};
  const info = modsTab.statusInfo(modsTab.normalize(doc).mods.find(x => x.key === 'airlock'));
  assert.equal(info.tone, 'warn');
  assert.equal(info.cause, 'its latest build failed: assets/x.decl: parse error; the last working version keeps serving until it builds');
});
test('additive: the map modal: installed at another build, declared nothing placed, no upgrade, clash choice', () => {
  const doc = clone(v3);
  doc.map.mods.push({key: 'airlock', id: 'airlock', version: '0.0.0', state: 'installed', placedEntities: [], placedCount: 0,
                     references: [], size: 0, attached: true, nothingPlaced: true});
  doc.map.mods.find(x => x.id === 'jo.lava-floor').state = 'otherBuild';
  doc.map.registryClashes = [{slot: 'entitydef/player/coop/edit/handsModelMPUseDoomHands', providers: [{id: 'k.a', name: 'Bare Hands'}, {id: 'k.b', name: 'Gloves'}],
                              chosen: '', serving: 'k.a'}];
  const h = live(doc);
  h.ui.openMap();
  const text = modalText(h);
  assert.doesNotMatch(text, /Needs Snapmap\+/);
  assert.match(text, /Declared, nothing placed/);
  assert.match(text, /not installed on this computer; playing the map downloads it/);
  assert.doesNotMatch(text, /another build/);
  /* an own map follows its folder in the backend: no row offers to move the map, whatever the document says */
  assert.ok(!modalButtons(h).find(b => /upgrade/i.test(b.textContent + ' ' + (b.title || ''))));
  assert.doesNotMatch(text, /upgrade|map uses build/i);
  /* the clash: one select per slot, its change written through the backend */
  const sel = h.nodes.modsMapHost.tags('select');
  assert.equal(sel.length, 1);
  assert.equal(sel[0].getAttribute('data-slot'), 'entitydef/player/coop/edit/handsModelMPUseDoomHands');
  assert.match(modalText(h), /edit\/handsModelMPUseDoomHands in entitydef\/player\/coop/);
  sel[0].value = 'k.b';
  sel[0].dispatch('change');
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'chooseRegistry', arg: 'entitydef/player/coop/edit/handsModelMPUseDoomHands\tk.b'});
  assert.equal(h.ui.state().model.map.clashes[0].serving, 'k.b', 'the page shows what the backend returned');
  assert.equal(modsTab.slotText('aifsmmanager/states/spidermastermind/edit/subGraphs/spiderMastermind'),
               'edit/subGraphs/spiderMastermind in aifsmmanager/states/spidermastermind');
});
/* V12: at the pane's 220 px minimum the Install button sat past the pane edge. The stand-in DOM
   cannot lay out, so the contract is the arithmetic the CSS promises: the label area (title and
   count) shrinks to nothing and is clipped, the buttons never shrink, and every button the header
   can show fits inside the pane's inset (measured in Chromium: work\fix2\L25\item1-header-geometry.txt). */
test('the header fits at the minimum pane width', () => {
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<div class="statusbar">'));
  const head = panel.slice(panel.indexOf('<div class="panel-head">'), panel.indexOf('<div class="panel-toolbar">'));
  assert.match(head, /<span class="mods-head-label"><span class="panel-title">Mods<\/span><span class="badge" id="modsBadge"[^>]*>0<\/span><\/span>/);
  const rule = sel => { const at = css.indexOf('\n  ' + sel + ' {'); assert.ok(at >= 0, sel); return css.slice(at, css.indexOf('}', at)); };
  assert.match(rule('.mods-head-label'), /flex: 1 1 0; min-width: 0; overflow: hidden;/);
  assert.match(rule('.mods-head-label > .panel-title'), /min-width: 0; overflow: hidden; text-overflow: ellipsis;/);
  assert.match(rule('#panel-mods .panel-head > .panel-actions'), /flex: none;/);
  const px = (text, re) => Number(text.match(re)[1]);
  const button = px(rule('.btn.icon'), /width: (\d+)px/);
  const gap = px(rule('.panel-actions'), /gap: (\d+)px/);
  const headGap = px(rule('.panel-head'), /gap: (\d+)px/);
  const inset = px(css, /--pane-inline: (\d+)px/);
  const min = Number(panel.match(/data-min-before="(\d+)"[^>]*data-split-label="Mods"/)[1]);
  const buttons = (head.match(/<button /g) || []).length;
  assert.equal(buttons, 6, 'every header button counted, the hidden ones too');
  assert.ok(2 * inset + headGap + buttons * button + (buttons - 1) * gap <= min,
            'six buttons fit in a ' + min + ' px pane');
});
test('page contract', () => {
  assert.ok(html.includes('<div class="tab" data-tab="mods">Mods</div>'));
  assert.ok(html.includes('<div class="content" id="panel-mods" style="display:none;">'));
  IDS.forEach(id => assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id));
  GONE.forEach(id => assert.ok(!html.includes('id="' + id + '"'), 'mockup.html no longer has #' + id));
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<div class="statusbar">'));
  assert.ok(!/This map<|mods-pill/.test(panel), 'This map left the details pane');
  /* the header's left side is icon buttons, as the Prefabs tab's (owner 2026-11-09); the toolbar holds the filter alone */
  const head = panel.slice(panel.indexOf('<div class="panel-head">'), panel.indexOf('<div class="panel-toolbar">'));
  const actions = head.slice(head.indexOf('<div class="panel-actions">'));
  for (const id of ['modsAttnBtn', 'modsMapBtn', 'modsRefreshBtn', 'modsFolderBtn', 'modsMigrateBtn', 'modsInstallBtn'])
    assert.match(actions, new RegExp('<button type="button" class="btn (primary )?icon icon-button( mods-attn)?" id="' + id + '"'), id + ' is a panel icon button');
  const pfHead = html.slice(html.indexOf('<span class="panel-title">Prefabs</span>'), html.indexOf('id="prefabFilter"'));
  assert.match(pfHead, /<div class="panel-actions">\n\s*<button type="button" class="btn icon icon-button"/, 'the same primitives as the Prefabs header');
  const toolbar = panel.slice(panel.indexOf('<div class="panel-toolbar">'), panel.indexOf('id="modsList"'));
  assert.ok(toolbar.includes('id="modsFilter"') && !/<button/.test(toolbar), 'the toolbar is the filter alone');
  assert.ok(!/class="badge mods-attn"|mods-toolbar/.test(panel), 'no pill-shaped attention badge');
  for (const [id, ic] of [['modsFolderBtn', 'folder-open'], ['modsMigrateBtn', 'folder-input'], ['modsInstallBtn', 'download'], ['modsMapBtn', 'map']])
    assert.match(panel, new RegExp('id="' + id + '"[^>]*>(<svg class="ui-icon"[^>]*><use href="#icon-' + ic + '">)'), id + ' is the ' + ic + ' icon');
  assert.match(panel, /id="modsMigrateBtn" style="display:none"/);
  assert.match(panel, /<button type="button" class="btn icon icon-button mods-attn" id="modsAttnBtn" aria-pressed="false" style="display:none"/);
  assert.ok(panel.indexOf('id="modsMapBtn"') < panel.indexOf('class="pane-splitter"'), 'the map button is in the left pane');
  for (const label of ['>Verify<', '>Rebuild<', '>Uninstall<', '>Use in this map<']) assert.ok(panel.includes(label), 'head action ' + label);
  assert.match(panel, /id="modsUseBtn" disabled style="display:none"/);
  for (const ic of ['check', 'triangle-alert', 'circle-info', 'folder-open', 'folder-input', 'map', 'chevron-down', 'x'])
    assert.ok(html.includes('<symbol id="icon-' + ic + '"'), 'icon ' + ic);
  /* the modal is the decl focus modal's markup */
  assert.match(html, /<div class="modal-overlay" id="modsMapModal" style="display:none;" aria-hidden="true">\n  <div class="modal decl-focus-modal mods-map-modal" role="dialog" aria-modal="true" aria-labelledby="modsMapTitle">\n    <div class="decl-focus-host mods-map-host" id="modsMapHost"><\/div>/);
  assert.match(html, /<div class="modal-overlay" id="declFocusModal" style="display:none;" aria-hidden="true">\n  <div class="modal decl-focus-modal"/);
  assert.match(html, /id="modsList" role="listbox" tabindex="0"/);
  assert.equal(html.split('<script src="mods_tab.js"></script>').length, 2, 'one module tag');
  assert.match(html, /function onMessage\(d\) \{\n    if \(typeof modsUi !== 'undefined' && modsUi\.onMessage\(d\)\) return;/);
  assert.match(html, /if \(which === 'mods'\) modsUi\.show\(\);/);
  /* the browser preview's data is the v3 fixture, answered by the stand-in host */
  const m = html.match(/\n  var PREVIEW_MODS = (\{.*\});  \/\/ migratedFrom names the retired package folder a mod was moved from\n/);
  assert.ok(m, 'PREVIEW_MODS is one JSON line');
  assert.deepEqual(JSON.parse(m[1]), v3, 'PREVIEW_MODS is the v3 fixture');
  assert.match(html, /var previewMods = modsTab\.standIn\(PREVIEW_MODS, PREVIEW_MOD_FILES\);/);
  const pf = html.match(/\n  var PREVIEW_MOD_FILES = (\{.*\});\r?\n/);
  assert.ok(pf, 'PREVIEW_MOD_FILES is one JSON line');
  const hgFiles = JSON.parse(pf[1])['local.hell-guard'];
  assert.ok(hgFiles.files.length > 200 && hgFiles.files.every(f => f.path && f.kind && 'class' in f), 'the preview lists files as modFiles does');
  /* the Files modal is the decl focus modal's markup; New mod is the standard small modal */
  assert.match(html, /<div class="modal-overlay" id="modsFilesModal" style="display:none;" aria-hidden="true">\n  <div class="modal decl-focus-modal mods-files-modal" role="dialog" aria-modal="true" aria-labelledby="modsFilesTitle">\n    <div class="decl-focus-host mf-host" id="modsFilesHost"><\/div>/);
  assert.match(html, /<div class="modal-overlay" id="modsNewModal" style="display:none;" aria-hidden="true">\n  <div class="modal" role="dialog"/);
  assert.match(panel, /id="modsFilesBtn"[^>]*><svg class="ui-icon"[^>]*><use href="#icon-folder-tree">/, 'Files is the folder-tree icon');
  assert.ok(panel.indexOf('id="modsFilesBtn"') < panel.indexOf('id="modsVerifyBtn"') &&
            panel.indexOf('id="modsFilesBtn"') > panel.indexOf('class="pane-splitter"'), 'Files sits in the details head beside Verify');
  for (const ic of ['folder-tree', 'gamepad-2']) assert.ok(html.includes('<symbol id="icon-' + ic + '"'), 'icon ' + ic);
  assert.match(html, /tokens: function\(text\)\{ var t = declLanguage\.tokenize\(text\); declLanguage\.parse\(t\); return t; \}/);
  assert.match(html, /if \(o\.cmd === 'modsAction'\) \{[\s\S]*?previewMods\(o\)\.forEach\(onMessage\);/);
  /* the retired collapsed-groups setting: the page neither asks for it nor saves it */
  assert.ok(!/MODS_COLLAPSED_KEY|saveCollapsed|loadCollapsed/.test(html));
  assert.ok(!/mods\.collapsed_groups'/.test(html), 'no request names the key');
  assert.ok(!/\.mods-group/.test(css), 'the group styles are gone');
  assert.ok(!/\.mods-pill/.test(css), 'no pill styles');
  assert.equal(html.split('localStorage').length - 1, 2, 'browser storage stays the preview theme only');
  assert.ok(!/setInterval|setTimeout|requestAnimationFrame/.test(source), 'the tab never polls or defers');
  assert.ok(!/innerHTML/.test(source), 'all text goes through textContent');
  assert.ok(!/[^\x00-\x7f]/.test(source), 'ASCII only');
  /* the scrolling and layout contract */
  assert.match(css, /\.panel-body \{ flex: 1; overflow-y: auto; min-height: 0; \}/);
  assert.ok(/\.mods-row \{[^}]*\}/.test(css) && !/\.mods-row \{[^}]*(position: absolute|height:)/.test(css), 'rows are in flow, no fixed height');

  assert.match(css, /\.btn\.danger-solid \{ background: var\(--danger\);/);
  assert.ok(!/mods-sizer|mods-shadow/.test(css), 'the windowed list and the shadow list are gone');
  /* the payload spec */
  for (const k of ['"description"', '"origin"', '"migratedFrom"', '"servedBy"', '"builtAt"', '"bundle"', '"counts"', '"requirements"',
                   '"editorRowNames"', '"carriedPaths"', '"publishedMaps"', '"placedEntities"', '"references"', '"vanilla"',
                   'mapRemoveModPreview', 'uninstallPreview', '`editorRows`', 'files minus pointers', '`wire`',
                   '"savedMaps"', '"undoCleared"', '"incomplete"', 'mapAddMod', '"attached"', '"scope"', '"scopeAuto"', '"active"'])
    assert.ok(docs.indexOf(k) >= 0, 'docs/webview-ui.md documents ' + k);
});
test('host and interface contract', () => {
  assert.match(iface, /sh_mods_fn\s+mods;\s+\/\* \+0x378 \(ext 34\) Mods tab \*\//);
  assert.match(iface, /SH_STATIC_ASSERT\(offsetof\(sh_iface_vtbl, mods\) == 0x378\);/);
  assert.match(iface, /#define SH_MODS_ACT_REMOVE      7\n#define SH_MODS_ACT_MIGRATE     8/);
  assert.ok(host.includes('} else if (cmd == L"modsAction") {'));
  assert.match(host, /else if \(code == SH_MODS_ACT_INSTALL\) poc_mods_begin\(code, true, std::string\(\)\);/, 'install takes its folder from the native picker only');
  assert.ok(host.includes('L"{\\"kind\\":\\"mods\\",\\"data\\":"'));
  assert.ok(host.includes('{\\"kind\\":\\"modsActionResult\\",\\"action\\":\\"'));
  assert.ok(host.includes('SH_IFACE_HAS_SLOT(g_iface, mods)'), 'an older backend is never called past its table');
  /* The host maps every action the page sends to the backend's code (openFolder is the host's own,
     with the folder name); only an unknown name is answered "Unknown mods action." */
  for (const a of ['install', 'verify', 'rescan', 'remove', 'migrate', 'rebuild', 'uninstall', 'uninstallPreview',
                   'mapRemoveMod', 'mapRemoveModPreview'])
    assert.ok(host.includes('if (name == L"' + a + '")'), 'host maps ' + a);
  for (const a of modsTab.ACTIONS.filter((x) => x !== 'openFolder'))
    assert.ok(host.includes('if (name == L"' + a + '")'), 'every page action reaches the backend: ' + a);
  assert.ok(host.includes('if (act == L"openFolder") poc_mods_open_folder(arg);'), 'openFolder opens the named mod folder');
  assert.match(iface, /#define SH_MODS_ACT_MAP_REMOVE_PREVIEW 13/);
  assert.match(iface, /#define SH_MODS_ACT_REMOVED_EMPTY      14/);
  assert.match(iface, /#define SH_MODS_ACT_MAP_ADD            16/);
  assert.ok(host.includes('case SH_MODS_ACT_MAP_ADD:            return L"mapAddMod";'));
  assert.ok(host.includes('poc_mods_post_result(L"unknown", false, L"Unknown mods action.", nullptr);'));
});

/* ---- organizing folders, the Files modal and New mod (L28) ------------------------------------ */
function grouped() {
  const d = clone(v3);
  for (const m of d.mods) if (['doors', 'elevators', 'tarps'].indexOf(m.key) >= 0) { m.group = 'campaign-interactables'; m.key = 'campaign-interactables\\' + m.key; }
  d.mods.find(m => m.key === 'campaign-interactables\\tarps').cause = 'its latest build failed';
  d.mods.find(m => m.key === 'campaign-interactables\\tarps').state = 'attention';
  return d;
}
test('organizing folders: a caret row with a count, its mods indented, closed for the session only', () => {
  const h = harness();
  shown(h, grouped());
  const list = h.nodes.modsList;
  const folders = list.all('mods-folder');
  assert.equal(folders.length, 1, 'one row per organizing folder');
  const f = folders[0];
  assert.equal(list.children[0], f, 'folders first');
  const head = f.all('pf-folder-head')[0];
  assert.equal(head.all('pf-fname')[0].textContent, 'campaign-interactables');
  assert.equal(head.all('pf-fcount')[0].textContent, '3');
  assert.equal(head.all('pf-caret').length, 1, 'the Prefabs caret');
  assert.deepEqual(f.all('mods-row').map(n => n.getAttribute('data-key')),
                   ['campaign-interactables\\doors', 'campaign-interactables\\elevators', 'campaign-interactables\\tarps'], 'its mods, in the list order');
  assert.ok(f.all('pf-folder-body')[0].all('mods-row').length === 3, 'indented in the folder body');
  assert.equal(rows(h).length, 36, 'every mod is still a row');
  assert.ok(list.children.slice(1).every(n => /mods-row/.test(n.className)), 'the root mods follow, flat');
  assert.ok(rows(h).every(n => n.all('mods-status-icon').length === 1 && n.all('mods-pill').length === 0), 'one glyph per row, no pills');
  assert.equal(head.all('mods-status-icon').length, 0, 'an open folder shows its rows, not a summary glyph');
  /* closing hides its rows and the arrow keys skip them; a problem inside shows on the closed row */
  head.click();
  const closed = h.nodes.modsList.all('mods-folder')[0];
  assert.ok(!/pf-open/.test(closed.className));
  assert.equal(closed.all('mods-row').length, 0);
  assert.equal(closed.all('pf-folder-head')[0].getAttribute('aria-expanded'), 'false');
  assert.equal(closed.all('mods-status-icon')[0].getAttribute('aria-label'), '1 mod needs attention');
  assert.ok(h.ui.state().view.every(b => !b.group), 'the keys walk the rows shown');
  assert.deepEqual(h.ui.state().closedGroups, ['campaign-interactables']);
  assert.equal(h.saved.length, 0, 'nothing is saved: the closed state lasts for the session');
  /* a new document keeps it closed; a filter shows the matches inside; selecting a mod inside opens it */
  h.ui.onMessage({kind: 'mods', data: grouped()});
  assert.equal(h.nodes.modsList.all('mods-folder')[0].all('mods-row').length, 0);
  h.nodes.modsFilter.value = 'elev';
  h.nodes.modsFilter.dispatch('input');
  assert.deepEqual(keys(h), ['campaign-interactables\\elevators']);
  h.nodes.modsFilter.value = '';
  h.nodes.modsFilter.dispatch('input');
  h.ui.select('campaign-interactables\\doors');
  assert.equal(h.nodes.modsList.all('mods-folder')[0].all('mods-row').length, 3, 'selecting a mod inside opens its folder');
  /* the v2 document and the v3 fixture have no group: one flat list */
  assert.equal(modsTab.normalize(v3).mods.every(b => b.group === ''), true);
});

const HG = 'assets/generated/decls/snapeditorentitydef/mods/local.hell-guard/demons/hell_guard_hammer_enc.decl';
const FOLDERS = {'local.hell-guard': {files: [
  {path: 'mod.json', size: 900, kind: 'text', class: 'descriptor', game: false, container: '', inFolder: true, cause: ''},
  {path: HG, size: 1200, kind: 'text', class: 'add', game: false, container: '', inFolder: true, cause: ''},
  {path: 'assets/generated/decls/entitydef/mods/local.hell-guard/broken.decl', size: 0, kind: 'text', class: '', game: false, container: '',
   inFolder: true, cause: 'empty file: assets/generated/decls/entitydef/mods/local.hell-guard/broken.decl'},
  {path: 'assets/generated/cm/models/mapobjects/hell_terrain/boulder_kit_f.bcm', size: 2048, kind: 'binary', class: 'surface', game: true,
   container: 'snap_gameresources', inFolder: false, cause: ''},
  {path: 'assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl', size: 300, kind: 'text', class: 'edit', game: false,
   container: '', inFolder: true, cause: ''}],
  texts: {'mod.json': '{\r\n  "name": "Hell Guards"\r\n}\r\n', [HG]: '{\r\n\tinherit = "demons/demon_base";\r\n}\r\n'}}};
test('the Files modal: the folder tree, one glyph per file, the file with its class and read-only text', () => {
  const h = harness({store: modsTab.standIn(v3, FOLDERS)});
  h.ui.show();
  h.ui.select('local.hell-guard');
  assert.equal(h.nodes.modsFilesBtn.disabled, false, 'Files needs a selected mod');
  h.nodes.modsFilesBtn.click();
  assert.equal(h.nodes.modsFilesModal.style.display, 'flex');
  assert.equal(h.nodes.modsFilesModal.getAttribute('aria-hidden'), 'false');
  assert.deepEqual(h.sent.filter(m => m.action === 'modFiles'), [{cmd: 'modsAction', action: 'modFiles', arg: 'local.hell-guard'}]);
  const st = h.ui.state();
  assert.equal(st.busy, '', 'a read never holds the page busy');
  assert.equal(st.files.open, true);
  /* the file a problem names is selected first, and its text asked for */
  assert.equal(st.files.selected, 'assets/generated/decls/entitydef/mods/local.hell-guard/broken.decl');
  const host = h.nodes.modsFilesHost;
  assert.equal(host.all('modal-title')[0].textContent, 'Hell Guards');
  assert.equal(host.all('mf-state')[0].textContent, 'Installed and serving 24 files, 476 more read from your install');
  assert.equal(host.all('mf-filter')[0].getAttribute('placeholder'), 'Filter files...');
  /* the tree: mod.json at the top, folders with counts, a closed folder with a problem inside marked */
  const list = host.all('mf-list')[0];
  assert.equal(list.children[0].getAttribute('data-path'), 'mod.json');
  const names = list.all('pf-fname').map(n => n.textContent);
  assert.ok(names.indexOf('assets/generated') >= 0, 'a folder holding one folder shows as one row');
  assert.ok(names.indexOf('entitydef') >= 0 && names.indexOf('mods/local.hell-guard') >= 0, 'the decl type keeps its row above mods/');
  assert.ok(names.indexOf('cm/models/mapobjects/hell_terrain') >= 0);
  const item = p => list.all('pf-item').find(n => n.getAttribute('data-path') === p);
  const broken = item('assets/generated/decls/entitydef/mods/local.hell-guard/broken.decl');
  assert.ok(/selected/.test(broken.className));
  assert.equal(broken.all('mf-g')[0].getAttribute('aria-label'), 'empty file: assets/generated/decls/entitydef/mods/local.hell-guard/broken.decl');
  assert.ok(/attn/.test(broken.all('mf-g')[0].className));
  /* the right side: the path once, the class phrase, the text read-only, the cause in the problem strip */
  const pane = host.all('mf-file')[0];
  assert.equal(pane.all('mf-path')[0].textContent, 'assets/generated/decls/entitydef/mods/local.hell-guard/ broken.decl');
  assert.equal(pane.all('mf-meta')[0].children[0].textContent, 'Not served yet');
  assert.equal(pane.all('prob')[0].textContent, 'empty file: assets/generated/decls/entitydef/mods/local.hell-guard/broken.decl');
  assert.ok(/problems has/.test(pane.all('problems')[0].className), 'the Entity State strip');
  /* the mod's own decl: no glyph, its text in the Decl Text editor primitive, read-only */
  assert.equal(item(HG), undefined, 'a folder off the selected file stays closed');
  h.ui.selectFile(HG);
  assert.deepEqual(h.sent[h.sent.length - 1], {cmd: 'modsAction', action: 'modFileText', arg: 'local.hell-guard\t' + HG});
  assert.equal(item(HG).all('mf-g').length, 0, "the mod's own file has no glyph");
  const ed = host.all('mf-file')[0].all('code-editor')[0];
  assert.ok(ed, 'the Decl Text editor');
  const ta = ed.tags('textarea')[0];
  assert.equal(ta.readOnly, true);
  assert.equal(ta.value, '{\n\tinherit = "demons/demon_base";\n}\n');
  assert.equal(ed.all('ce-gutter')[0].children[0].textContent, '1\n2\n3', 'one number per line');
  assert.equal(host.all('mf-meta')[0].children[0].textContent, 'New file');
  assert.equal(host.all('prob').length, 0, 'no problem, no strip');
  /* a game file: the game glyph, muted, its archive named, binary: class, size and source, no editor */
  const bcm = 'assets/generated/cm/models/mapobjects/hell_terrain/boulder_kit_f.bcm';
  const sent = h.sent.length;
  h.ui.selectFile(bcm);
  assert.equal(h.sent.length, sent, 'nothing to read for a binary file');
  assert.ok(/mf-ro/.test(item(bcm).className));
  assert.equal(item(bcm).all('mf-g')[0].children[0].children[0].getAttribute('href'), '#icon-gamepad-2');
  assert.deepEqual(host.all('mf-meta')[0].children.map(n => n.textContent),
                   ['A game file SnapMap cannot see on its own', "from the game's SnapMap files", '2.0 KB']);
  assert.equal(host.all('mf-file')[0].all('code-editor').length, 0);
  /* an edit of a game file is the author's: no game glyph */
  h.ui.selectFile('assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl');
  assert.equal(item('assets/generated/decls/entitydef/ai/conductor/coop/snapmap_default.decl').all('mf-g').length, 0);
  assert.equal(host.all('mf-meta')[0].children[0].textContent, 'Your edit of a game file');
  /* mod.json without its product block */
  item('mod.json').click();
  assert.match(host.all('mf-file')[0].textContent, /Snapmap\+ writes the rest of mod\.json itself/);
  assert.equal(host.all('mf-meta')[0].children[0].textContent, modsTab.FILE_CLASSES.descriptor);
  /* the filter keeps matching files and opens their folders */
  const input = host.all('mf-filter')[0];
  input.value = 'boulder';
  input.dispatch('input');
  assert.deepEqual(host.all('mf-list')[0].all('pf-item').map(n => n.getAttribute('data-path')), [bcm]);
  assert.equal(h.ui.state().files.selected, 'mod.json', 'filtering does not change the file shown');
  /* Escape closes; focus returns to the Files button */
  h.nodes.modsFilesModal.dispatch('keydown', {key: 'Escape'});
  assert.equal(h.nodes.modsFilesModal.style.display, 'none');
  assert.equal(activeElement, h.nodes.modsFilesBtn);
  assert.equal(h.nodes.modsFilesHost.children.length, 0);
});
test('the Files modal: a refusal and a late answer stay in the modal; the status line is untouched', () => {
  const h = harness();
  shown(h);
  h.ui.select('local.hell-guard');
  h.nodes.modsFilesBtn.click();
  h.ui.onMessage({kind: 'modsActionResult', action: 'modFiles', ok: false, message: 'Another mods action is still running.', data: null});
  assert.match(h.nodes.modsFilesHost.textContent, /Another mods action is still running\./);
  assert.equal(h.toasts.length, 0, 'no toast for a read');
  assert.equal(h.nodes.modsStatus.textContent, '');
  /* an answer for another mod is not this modal's */
  h.ui.onMessage({kind: 'modsActionResult', action: 'modFiles', ok: true, message: '', data: {key: 'airlock', files: [{path: 'mod.json'}]}});
  assert.equal(h.ui.state().files.data, null);
  /* a text refusal answers the file last asked for */
  h.ui.openFiles('local.hell-guard');
  h.ui.onMessage({kind: 'modsActionResult', action: 'modFiles', ok: true, message: '', data: {key: 'local.hell-guard', name: 'Hell Guards', cause: '',
    files: [{path: 'mod.json', kind: 'text', class: 'descriptor', inFolder: true}]}});
  assert.equal(h.ui.state().files.selected, 'mod.json');
  h.ui.onMessage({kind: 'modsActionResult', action: 'modFileText', ok: false, message: 'mod.json could not be read; close any program that holds it and try again.', data: null});
  assert.match(h.nodes.modsFilesHost.textContent, /close any program that holds it/);
  assert.equal(h.ui.state().busy, '');
});
test('the Files view model: classes, sources and the tree', () => {
  const d = modsTab.normFiles({key: 'k', files: [{path: 'b\\x.decl', kind: 'text', class: 'mystery', container: 'mars', game: 'yes'},
                                                  {path: ''}, {path: 'a.json', kind: 'text', class: 'descriptor'}]});
  assert.deepEqual(d.files.map(f => [f.path, f.cls, f.container, f.game, f.text]), [['a.json', 'descriptor', '', false, true], ['b/x.decl', '', '', false, true]]);
  assert.deepEqual(modsTab.fileMeta({cls: 'borrow', game: true, container: 'gameresources', text: true, inFolder: false, size: 10}),
                   ["A game file under this mod's own name", "from the game's campaign and multiplayer files", '10 B']);
  const t = modsTab.fileTree([{path: 'assets/generated/decls/entitydef/mods/k/a.decl', cause: 'x'},
                              {path: 'assets/generated/decls/entitydef/mods/k/b.decl', cause: ''},
                              {path: 'assets/generated/decls/material/mods/k/c.decl', cause: ''}], '');
  assert.equal(t.dirs[0].name, 'assets/generated/decls');
  assert.deepEqual(t.dirs[0].dirs.map(x => [x.name, x.count, x.attn]), [['entitydef', 2, 1], ['material', 1, 0]]);
  assert.equal(t.dirs[0].dirs[0].dirs[0].name, 'mods/k');
  assert.equal(modsTab.fileTree([{path: 'a/b.decl'}], 'zzz').count, 0);
  for (const v of Object.values(modsTab.FILE_CLASSES).concat(Object.values(modsTab.FILE_SOURCES), [modsTab.NEW_MOD_HINT]))
    assert.ok(!/\bbuild|legacy|bundle\b/i.test(v), 'no build or legacy vocabulary: ' + v);
});
test('New mod: a name, the folder made with its example, the mod selected, Files open on its Toybox row', () => {
  const h = harness({store: modsTab.standIn(v3)});
  h.ui.show();
  /* the list's right-click entry: the header has no room for a seventh button at 220 px */
  const e = h.nodes.modsList.dispatch('contextmenu', {pageX: 40, pageY: 90});
  assert.equal(e.defaultPrevented, true);
  assert.deepEqual(h.menus.map(m => m.items.map(i => i.label)), [['New mod...']]);
  h.menus[0].items[0].run();
  assert.equal(h.nodes.modsNewModal.style.display, 'flex');
  assert.equal(h.nodes.modsNewNote.textContent, "Snapmap+ makes a folder for it in mods\\ with an example crate you can place right away.");
  assert.equal(activeElement, h.nodes.modsNewName);
  /* an empty name is not sent */
  h.nodes.modsNewName.value = '   ';
  h.nodes.modsNewCreate.click();
  assert.ok(!h.sent.some(m => m.action === 'newMod'));
  assert.equal(h.toasts.pop().text, 'Give the new mod a name.');
  h.nodes.modsNewName.value = 'My first mod';
  h.nodes.modsNewModal.dispatch('keydown', {key: 'Enter'});
  assert.deepEqual(h.sent.filter(m => m.action === 'newMod'), [{cmd: 'modsAction', action: 'newMod', arg: 'My first mod'}]);
  assert.equal(h.nodes.modsNewModal.style.display, 'none');
  const st = h.ui.state();
  assert.equal(st.selectedKey, 'my_first_mod_k3x9', 'the list selects the new mod');
  assert.ok(rowOf(h, 'my_first_mod_k3x9'), 'the fresh document lists it');
  assert.equal(st.files.open, true);
  const row = 'assets/generated/decls/snapeditorentitydef/mods/my_first_mod_k3x9/example_crate.decl';
  assert.equal(st.files.selected, row, 'Files opens on the example Toybox row');
  const pane = h.nodes.modsFilesHost.all('mf-file')[0];
  assert.equal(pane.all('mf-hint')[0].textContent, modsTab.NEW_MOD_HINT, 'one quiet line naming the three steps');
  assert.match(pane.tags('textarea')[0].value, /entityDef = "mods\/my_first_mod_k3x9\/example_crate";/);
  assert.equal(h.nodes.modsFilesHost.all('modal-title')[0].textContent, 'My first mod');
  /* the hint is the new mod's only: another file has none */
  h.ui.selectFile('mod.json');
  assert.equal(h.nodes.modsFilesHost.all('mf-file')[0].all('mf-hint').length, 1, 'only the product-block note');
  assert.ok(!/Place Example crate/.test(h.nodes.modsFilesHost.textContent));
});
test('New mod from the empty list: the card holds the button', () => {
  const h = harness({store: modsTab.standIn(Object.assign(clone(v3), {mods: []}))});
  h.ui.show();
  const card = h.nodes.modsList.all('mods-empty')[0];
  assert.match(card.textContent, /No mods installed/);
  const b = card.all('mods-new-btn')[0];
  assert.equal(b.textContent, 'New mod');
  b.click();
  assert.equal(h.nodes.modsNewModal.style.display, 'flex');
  h.nodes.modsNewCancel.click();
  assert.equal(h.nodes.modsNewModal.style.display, 'none');
  assert.equal(activeElement, b, 'focus returns to the button');
});
test('the Files modal highlights with the decl tokens when the page supplies them', () => {
  const tokens = text => [{type: 'punc', start: 0, end: 1}, {type: 'ident', isKey: true, start: 3, end: 10}, {type: 'str', start: 13, end: 32}];
  const h = harness({store: modsTab.standIn(v3, FOLDERS), tokens});
  h.ui.show();
  h.ui.openFiles('local.hell-guard', HG);
  const code = h.nodes.modsFilesHost.all('code-editor')[0].tags('code')[0];
  assert.deepEqual(code.children.filter(n => n.className).map(n => [n.className, n.textContent]),
                   [['tk-punc', '{'], ['tk-key', 'inherit'], ['tk-str', '"demons/demon_base"']]);
});

console.log('mods_tab_test: OK (' + passed + ' tests)');
