/* The Mods tab (mods_tab.js): the payload model (v3, and v2 from a backend that predates it), the
   three row states, the origin groups and their persisted collapse, the attention filter, the details
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
             'modsMapBtn', 'modsMapModal', 'modsMapHost', 'modsUseBtn'];
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
  const sent = [], toasts = [];
  const saved = [];
  const doc = {createElement: tag => new FakeNode(tag), createElementNS: (ns, tag) => Object.assign(new FakeNode(tag), {ns})};
  let ui = null;
  ui = modsTab.create({
    document: doc, byId: id => nodes[id] || null, saveCollapsed: list => saved.push(list),
    post: m => { sent.push(m); if (opts.store) opts.store(m).forEach(r => ui.onMessage(r)); },
    toast: (t, k) => toasts.push({text: t, kind: k})
  });
  ui.bind();
  return {ui, nodes, sent, toasts, saved};
}
function rows(h) { return h.nodes.modsList.all('mods-row'); }
function rowOf(h, key) { return rows(h).find(n => n.getAttribute('data-key') === key); }
function groups(h) { return h.nodes.modsList.all('mods-group'); }
function groupHead(h, id) { return h.nodes.modsList.all('mods-group-head').find(n => n.getAttribute('data-group') === id); }
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
  assert.equal(hg.counts.shadows, 500);
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
  assert.equal(modsTab.normPreview({action: 'uninstall', install: {disposition: 'shred'}}).install.disposition, 'delete');
  const pv = modsTab.normPreview({action: 'mapRemoveMod', map: {references: [{kind: 'toybox', name: 'a'}, {kind: 'module', name: 'b'},
                                  {kind: 'wire', name: 'c'}, {kind: 'timeline', name: 'd'}]}});
  assert.deepEqual(pv.map.references.map(r => r.kind), ['wire', 'timeline'], 'only wires and timeline references are severed');
  assert.deepEqual(pv.editorRows, []);
});
test('three states: every backend state and cause maps onto check, triangle or info', () => {
  const s = b => modsTab.statusInfo(Object.assign({state: 'installed', files: 3, cause: '', duplicateId: false, servedBy: ''}, b));
  assert.deepEqual([s({}).tone, s({}).icon], ['ok', 'check']);
  assert.equal(s({}).sentence, 'Installed and serving 3 files');
  assert.equal(s({files: 1}).sentence, 'Installed and serving 1 file');
  assert.equal(s({files: 500, pointers: 476}).sentence, 'Installed and serving 24 files, 476 more read from your install',
    'carried files only (files minus pointers), the pointers as a second clause');
  assert.equal(modsTab.servingText({files: 2, pointers: 1}), 'Installed and serving 1 file, 1 more read from your install');
  const dup = s({state: 'attention', duplicateId: true});
  assert.deepEqual([dup.tone, dup.icon], ['warn', 'triangle-alert']);
  assert.equal(dup.sentence, 'Needs attention: another installed mod declares this id; change one id in its mod.json');
  assert.equal(s({duplicateId: true, servedBy: 'x'}).tone, 'warn', 'attention wins over a superseded build');
  for (const cause of ['not in the serving index yet; it is used after the next scan', 'skipped by the serving index',
                       'not built yet: bundle.json is missing', 'mod.json unreadable'])
    assert.equal(s({state: 'attention', cause}).cause, cause);
  assert.equal(s({state: 'attention'}).cause, 'not in use');
  const info = s({servedBy: 'local.cyberdemon@0.0.0-05238b2e'});
  assert.deepEqual([info.tone, info.icon], ['info', 'circle-info']);
  assert.match(info.sentence, /^Installed; another build of this mod \(local\.cyberdemon@0\.0\.0-05238b2e\) serves its files\.$/);
  assert.deepEqual(Object.keys(modsTab.STATUS), ['ok', 'warn', 'info']);
  assert.equal(modsTab.mapModStatus({state: 'missing'}).cause, 'not installed on this machine');
  assert.equal(modsTab.mapModStatus({state: 'active'}).tone, 'ok');
  assert.equal(modsTab.mapModStatus({state: 'superseded'}).tone, 'info');
});
test('groups and origins', () => {
  assert.equal(modsTab.groupOf({origin: ''}), 'yours');
  assert.equal(modsTab.groupOf({origin: 'migration'}), 'yours', 'migrated overrides were the owner\'s');
  assert.equal(modsTab.groupOf({origin: 'install'}), 'installed');
  assert.equal(modsTab.groupOf({origin: 'download'}), 'maps');
  assert.equal(modsTab.groupOf({origin: 'delivery'}), 'maps');
  assert.equal(modsTab.originText({origin: ''}), 'You added this folder');
  assert.equal(modsTab.originText({origin: 'install'}), 'Installed from a folder');
  assert.equal(modsTab.originText({origin: 'migration', migratedFrom: 'overrides\\hell-guards'}), 'Moved from overrides\\hell-guards');
  assert.equal(modsTab.originText({origin: 'migration', migratedFrom: ''}), 'Moved from the overrides folder');
  assert.equal(modsTab.originText({origin: 'download'}), 'Downloaded for a map');
  assert.equal(modsTab.originText({origin: 'delivery'}), 'Delivered by a map');
  const g = modsTab.groupMods(modsTab.normalize(v3).mods);
  assert.deepEqual(g.map(x => [x.label, x.mods.length]), [['Your mods', 30], ['Installed', 4], ['From maps you played', 2]]);
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
                                     'mapRemoveModPreview', 'mapRemoveMod', 'uninstallPreview', 'uninstall']);
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
  assert.match(h.nodes.modsList.textContent, /Reading installed mods/);
  answer(h, v3);
  assert.equal(h.toasts.length, 0, 'a successful rescan on show says nothing');
  h.ui.show();
  assert.equal(h.sent.length, 2, 'showing the tab again rereads the folder');
  assert.equal(h.ui.onMessage({kind: 'other'}), false);
});
test('the list: origin groups with counts, one status icon per row, two lines per row', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsBadge.textContent, '36');
  assert.deepEqual(groups(h).map(g => g.getAttribute('aria-label')), ['Your mods', 'Installed', 'From maps you played']);
  assert.deepEqual(h.nodes.modsList.all('pf-fcount').map(n => n.textContent), ['30', '4', '2']);
  assert.equal(rows(h).length, 36, 'every row of an open group is mounted');
  assert.ok(rows(h).every(n => n.style.top === undefined), 'rows are in flow');
  const hg = rowOf(h, 'local.hell-guard');
  assert.equal(hg.all('mods-row-name')[0].textContent, 'Hell Guards');
  assert.equal(hg.all('mods-row-sub')[0].textContent, 'local.hell-guard \u00b7 0.0.0 \u00b7 1.2 MB');
  assert.ok(!/files|installed game/.test(hg.textContent), 'the file counts left the row');
  assert.equal(hg.all('mods-pill').length, 0, 'no pills');
  assert.equal(iconOf(hg), 'ok');
  assert.equal(hg.title, 'Installed and serving 24 files, 476 more read from your install');
  const icon = hg.all('mods-status-icon')[0];
  assert.equal(icon.children[0].ns, 'http://www.w3.org/2000/svg');
  assert.equal(icon.children[0].children[0].getAttribute('href'), '#icon-check');
  for (const key of ['local.cyberdemon@0.0.0', 'local.cyberdemon@0.0.0-05238b2e']) {
    assert.equal(iconOf(rowOf(h, key)), 'warn');
    assert.equal(rowOf(h, key).title, 'another installed mod declares this id; change one id in its mod.json');
  }
  assert.equal(rowOf(h, 'local.cyberdemon@0.0.0').all('mods-status-icon')[0].children[0].children[0].getAttribute('href'), '#icon-triangle-alert');
  const maps = groups(h)[2];
  assert.deepEqual(maps.all('mods-row').map(n => n.getAttribute('data-key')), ['acceptance.mixed-rows', 'female.models']);
  assert.ok(groups(h)[1].all('mods-row').some(n => n.getAttribute('data-key') === 'test.shader-tint'));
  /* the grey info state */
  const sup = clone(v3);
  sup.mods.find(b => b.key === 'local.cyberdemon@0.0.0').duplicateId = false;
  sup.mods.find(b => b.key === 'local.cyberdemon@0.0.0').state = 'installed';
  h.ui.onMessage({kind: 'mods', data: sup});
  const grey = rowOf(h, 'local.cyberdemon@0.0.0');
  assert.equal(iconOf(grey), 'info');
  assert.equal(grey.all('mods-status-icon')[0].children[0].children[0].getAttribute('href'), '#icon-circle-info');
  assert.match(grey.title, /another build of this mod \(local\.cyberdemon@0\.0\.0-05238b2e\) serves its files/);
});
test('group collapse is a saved setting and is applied when it arrives', () => {
  assert.equal(modsTab.COLLAPSE_KEY, 'mods.collapsed_groups');
  const h = harness();
  shown(h);
  groupHead(h, 'yours').click();
  assert.equal(groupHead(h, 'yours').getAttribute('aria-expanded'), 'false');
  assert.equal(rows(h).length, 6, 'a collapsed group mounts no rows');
  assert.deepEqual(h.saved.pop(), ['yours']);
  /* keyboard: Enter and Space on the header toggle it */
  const e = groupHead(h, 'installed').dispatch('keydown', {key: 'Enter'});
  assert.equal(e.defaultPrevented, true);
  assert.deepEqual(h.saved.pop(), ['installed', 'yours']);
  groupHead(h, 'installed').dispatch('keydown', {key: ' '});
  assert.deepEqual(h.saved.pop(), ['yours']);
  /* a new page applies the stored value when the configuration answers; arrows skip the group */
  const again = harness();
  shown(again);
  again.ui.loadCollapsed(['yours', 42, '']);
  assert.deepEqual(again.ui.state().collapsed, ['yours']);
  assert.equal(rows(again).length, 6);
  again.nodes.modsList.dispatch('keydown', {key: 'Home'});
  assert.equal(again.ui.state().selectedKey, 'p6probe');
  /* a filter shows matches in every group without touching the saved set */
  again.nodes.modsFilter.value = 'hell';
  again.nodes.modsFilter.dispatch('input');
  assert.deepEqual(rows(again).map(n => n.getAttribute('data-key')), ['local.hell-guard']);
  groupHead(again, 'yours').click();
  assert.equal(rows(again).length, 0, 'a header still folds while filtering');
  assert.equal(again.saved.length, 0, 'and that fold is not saved');
  again.nodes.modsFilter.value = 'zzz';
  again.nodes.modsFilter.dispatch('input');
  assert.match(again.nodes.modsList.textContent, /No installed mod matches/);
  /* a bad stored value starts open */
  again.ui.loadCollapsed('nope');
  assert.deepEqual(again.ui.state().collapsed, []);
});
test('the attention badge filters to the rows that need attention', () => {
  const h = harness();
  assert.equal(h.nodes.modsAttnBtn.style.display, 'none', 'hidden before the first answer');
  shown(h);
  const b = h.nodes.modsAttnBtn;
  assert.equal(b.tagName, 'button', 'a real button: focusable, Enter and Space work');
  assert.equal(b.style.display, '');
  assert.match(b.textContent, /^2$/);
  assert.equal(b.getAttribute('aria-pressed'), 'false');
  assert.match(b.className, /badge mods-attn/);
  b.click();
  assert.equal(b.getAttribute('aria-pressed'), 'true');
  assert.match(b.className, / on$/, 'the pressed state shows');
  assert.deepEqual(rows(h).map(n => n.getAttribute('data-key')), ['local.cyberdemon@0.0.0', 'local.cyberdemon@0.0.0-05238b2e']);
  assert.deepEqual(groups(h).map(g => g.getAttribute('data-group')), ['yours']);
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
test('keyboard selection over the grouped list', () => {
  const h = harness();
  shown(h);
  rowOf(h, 'alex.r2-imp').click();
  assert.equal(h.ui.state().selectedKey, 'alex.r2-imp');
  assert.equal(rowOf(h, 'alex.r2-imp').getAttribute('aria-selected'), 'true');
  let e = h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(e.defaultPrevented, true);
  assert.equal(h.ui.state().selectedKey, 'spencer', 'next row in display order: R2 Imp copy, then Robots');
  assert.ok(rowOf(h, 'spencer').scrolledIntoView > 0);
  h.nodes.modsList.dispatch('keydown', {key: 'End'});
  assert.equal(h.ui.state().selectedKey, 'female.models');
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(h.ui.state().selectedKey, 'female.models');
  h.nodes.modsList.dispatch('keydown', {key: 'Home'});
  assert.equal(h.ui.state().selectedKey, 'airlock');
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
  assert.deepEqual(d.all('mods-section').map(n => n.getAttribute('data-section')), ['about', 'adds', 'files', 'requirements', 'sharing']);
  const about = section(h, 'about').textContent;
  for (const s of ['Id local.hell-guard', 'Version 0.0.0', 'Build 2', 'Description The three Hell Guard brothers',
                   'Folder local.hell-guard', 'Origin Moved from overrides\\hell-guards'])
    assert.ok(about.indexOf(s) >= 0, s + ' in ' + about);
  assert.ok(!/Author/.test(about), 'an empty author is left out');
  const adds = section(h, 'adds').textContent;
  assert.match(adds, /3 Toybox rows Hell Guard \(hammer\) Hell Guard \(shield\) Hell Guard \(fist\)/);
  assert.match(adds, /Replaces 500 stock files/);
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
  assert.match(req, /g_useImageBlackList 0 g_useResourceBlackList 0/);
  assert.ok(req.indexOf(modsTab.NEEDS_NOTE) >= 0);
  const share = section(h, 'sharing').textContent;
  assert.match(share, /Used by 2 of your published maps/);
  assert.match(share, /Last built \d{4}-\d\d-\d\d \d\d:\d\d/);
  assert.ok(share.indexOf('Bundle ' + v3.mods.find(m => m.key === 'local.hell-guard').bundle.slice(0, 16)) >= 0);
  assert.match(share, /when you ask for help/);
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
    'Needs attention: another installed mod declares this id; change one id in its mod.json');
  assert.match(h.nodes.modsDetail.all('mods-detail-sentence')[0].className, /warn/);
  /* Rebuild and Verify carry the key */
  h.nodes.modsRebuildBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'rebuild', arg: 'local.cyberdemon@0.0.0-05238b2e'});
  assert.match(h.nodes.modsStatus.textContent, /Building the mod again/);
  h.ui.onMessage({kind: 'modsActionResult', action: 'rebuild', data: {action: 'rebuild', ok: true, message: 'Built again.'}});
  h.nodes.modsVerifyBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'verify', arg: 'local.cyberdemon@0.0.0-05238b2e'});
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
  assert.match(req, /Console settings com_maxFps 120/);
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
  assert.match(section(h, 'about').textContent, /Origin You added this folder/);
  assert.match(section(h, 'adds').textContent, /^What it adds 2 Toybox rows$/);
  assert.equal(section(h, 'files').tags('details').length, 0, 'no carried list without carriedPaths');
  assert.match(section(h, 'files').textContent, /Carried 13 files, 1\.8 MB/);
  assert.equal(section(h, 'requirements').textContent, 'Requirements ' + modsTab.NEEDS_NOTE);
  rowOf(h, 'unbuilt-folder').click();
  assert.match(d.textContent, /Needs attention: not built yet: bundle\.json is missing/);
  assert.match(section(h, 'about').textContent, /Id \(not built yet\)/);
  assert.match(section(h, 'adds').textContent, /Adds no Toybox rows/);
  assert.equal(rowOf(h, 'unbuilt-folder').all('mods-row-sub')[0].textContent, 'unbuilt-folder');
  /* v2 has no origins: everything is Your mods */
  assert.deepEqual(groups(h).map(g => g.getAttribute('data-group')), ['yours']);
  assert.equal(h.nodes.modsAttnBtn.textContent, '4');
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
  assert.match(mr[4].textContent, /jo\.lava-floor .* 2 placed entities use it; not installed on this machine/);
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
  const removeBtn = () => h.nodes.modsMapHost.all('mods-map-row')[0].tags('button')[0];
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
  hand.nodes.modsMapHost.all('mods-map-row')[1].tags('button')[0].click();
  hand.ui.onMessage({kind: 'modsActionResult', action: 'mapRemoveModPreview', ok: true, data: {ok: true, preview: {action: 'mapRemoveMod', key: 'doors', name: 'All doors',
    map: {name: 'Hell Guard Arena', entities: ['Blast door 1'], entityCount: 1, references: [], manifestRow: true}}}});
  assert.deepEqual(hand.nodes.modsMapHost.tags('li').map(n => n.textContent), ['Deletes 1 placed entity from Hell Guard Arena. Blast door 1',
    "Removes the mod's row from the map's list of mods."], 'only what the dry run says');
  modalButtons(hand)[1].click();
  hand.ui.onMessage({kind: 'modsActionResult', action: 'mapRemoveMod', ok: true, data: {ok: true, message: 'Removed.'}});
  assert.equal(hand.nodes.modsMapHost.all('mods-map-row').length, 5, 'still the last document');
  /* a refused dry run says why and stays on the list */
  hand.nodes.modsMapHost.all('mods-map-row')[1].tags('button')[0].click();
  hand.ui.onMessage({kind: 'modsActionResult', action: 'mapRemoveModPreview', ok: false, message: 'The editor is busy.', data: null});
  assert.deepEqual(hand.toasts.pop(), {text: 'The editor is busy.', kind: 'err'});
  assert.equal(hand.ui.state().modal.page, 'list');
});
test('the last removal leaves a vanilla map', () => {
  const h = live();
  h.nodes.modsMapBtn.click();
  for (let n = 5; n > 1; n--) {
    h.nodes.modsMapHost.all('mods-map-row')[0].tags('button')[0].click();
    modalButtons(h)[1].click();
  }
  h.nodes.modsMapHost.all('mods-map-row')[0].tags('button')[0].click();
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
  assert.ok(items.indexOf('Deletes the folder mods\\doors and its files.') >= 0, 'Uninstall deletes the folder');
  assert.equal(items[items.length - 1], modsTab.UNDO_CLEARED);
  assert.equal(modsTab.UNDO_CLEARED, 'Undo history is cleared.');
  assert.deepEqual(modalButtons(h).map(b => b.textContent), ['Cancel', 'Uninstall']);
  assert.equal(h.nodes.modsMapHost.all('mods-scope')[0].textContent, 'Removes the mod from this computer. The open map is cleaned of it first.');
  assert.equal(h.nodes.modsMapHost.all('mods-note')[1].textContent, modsTab.UNINSTALL_PERMANENT);
  assert.equal(modsTab.UNINSTALL_PERMANENT, 'This cannot be undone. Uninstalling:');
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
                           'Deletes the folder mods\\alex.mp-basin and its files.',
                           modsTab.UNDO_CLEARED]);
  assert.equal(h.nodes.modsMapHost.all('mods-scope')[0].textContent, 'Removes the mod from this computer. The open map does not use it.');
  assert.equal(h.nodes.modsMapHost.all('mods-note')[1].textContent, modsTab.UNINSTALL_PERMANENT);
  modalButtons(h)[1].click();
  assert.equal(rowOf(h, 'alex.mp-basin'), undefined);
  /* uninstalling one of two duplicates clears the other's attention */
  rowOf(h, 'local.cyberdemon@0.0.0').click();
  h.nodes.modsUninstallBtn.click();
  modalButtons(h)[1].click();
  assert.equal(iconOf(rowOf(h, 'local.cyberdemon@0.0.0-05238b2e')), 'ok');
  assert.equal(h.nodes.modsAttnBtn.style.display, 'none');
});
test('Uninstall is permanent: saved maps that use it, the undo history, no bin', () => {
  /* the dry run's savedMaps and undoCleared become the confirmation's lines */
  const pv = modsTab.normPreview({action: 'uninstall', install: {folder: 'hell-guards', disposition: 'move', savedMaps: 3, undoCleared: true}});
  assert.deepEqual(pv.install, {folder: 'hell-guards', disposition: 'delete', savedMaps: 3, undoCleared: true});
  assert.deepEqual(modsTab.consequenceLines(pv).map(c => c.text), [
    'Deletes the folder mods\\hell-guards and its files.',
    '3 of your saved maps use it; they will not open until a mod folder provides its assets again.',
    'Undo history is cleared.']);
  /* none of them, or the saves could not be read: no line; no map open: the history is left alone */
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall', install: {folder: 'a', savedMaps: 0}})).map(c => c.text),
    ['Deletes the folder mods\\a and its files.']);
  assert.equal(modsTab.normPreview({action: 'uninstall', install: {folder: 'a'}}).install.savedMaps, -1);
  /* an uninstall's result says what happened; the footer has no bin line */
  const h = live();
  rowOf(h, 'alex.mp-basin').click();
  h.nodes.modsUninstallBtn.click();
  modalButtons(h)[1].click();
  assert.match(h.toasts.slice(-1)[0].text, /^Uninstalled .*\. Its folder was deleted and the undo history was cleared\.$/);
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
test('scope: where a mod applies is in About only when the backend says so', () => {
  const doc = clone(v3);
  const by = k => doc.mods.find(m => m.key === k);
  by('airlock').scope = 'map'; by('airlock').scoped = true;
  by('alex.r2-imp').scope = 'map'; by('alex.r2-imp').scoped = false;
  by('crucible').scope = 'always'; by('crucible').scopeAuto = true;
  by('commdish').scope = 'always';
  const n = modsTab.normalize(doc), m = k => n.mods.find(x => x.key === k);
  assert.equal(modsTab.scopeText(m('airlock')), 'Only in maps that use it');
  assert.equal(modsTab.scopeText(m('alex.r2-imp')), '', 'new names only: nothing to say');
  assert.equal(modsTab.scopeText(m('crucible')), 'In every map: it changes the editor itself');
  assert.equal(modsTab.scopeText(m('commdish')), 'In every map');
  assert.equal(modsTab.scopeText(m('dlc')), '', 'an older backend sends no scope');
  const h = live(doc);
  h.ui.select('airlock');
  assert.match(section(h, 'about').textContent, /Applies Only in maps that use it/);
  h.ui.select('dlc');
  assert.ok(!/Applies/.test(section(h, 'about').textContent));
});
test('page contract', () => {
  assert.ok(html.includes('<div class="tab" data-tab="mods">Mods</div>'));
  assert.ok(html.includes('<div class="content" id="panel-mods" style="display:none;">'));
  IDS.forEach(id => assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id));
  GONE.forEach(id => assert.ok(!html.includes('id="' + id + '"'), 'mockup.html no longer has #' + id));
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<div class="statusbar">'));
  assert.ok(!/This map<|mods-pill/.test(panel), 'This map left the details pane');
  const toolbar = panel.slice(panel.indexOf('<div class="panel-toolbar mods-toolbar">'), panel.indexOf('id="modsList"'));
  for (const id of ['modsFilter', 'modsRefreshBtn', 'modsFolderBtn', 'modsMigrateBtn', 'modsInstallBtn']) assert.ok(toolbar.includes('id="' + id + '"'), 'one toolbar row has ' + id);
  for (const [id, ic] of [['modsFolderBtn', 'folder-open'], ['modsMigrateBtn', 'folder-input'], ['modsInstallBtn', 'download'], ['modsMapBtn', 'map']])
    assert.match(panel, new RegExp('id="' + id + '"[^>]*>(<svg class="ui-icon"[^>]*><use href="#icon-' + ic + '">)'), id + ' is the ' + ic + ' icon');
  assert.match(panel, /id="modsMigrateBtn" style="display:none"/);
  assert.match(panel, /<button type="button" class="badge mods-attn" id="modsAttnBtn" aria-pressed="false"/);
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
  assert.match(html, /var previewMods = modsTab\.standIn\(PREVIEW_MODS\);/);
  assert.match(html, /if \(o\.cmd === 'modsAction'\) \{[\s\S]*?previewMods\(o\)\.forEach\(onMessage\);/);
  /* the collapsed groups are a configuration setting, never browser storage */
  assert.match(html, /var MODS_COLLAPSED_KEY = 'mods\.collapsed_groups';/);
  assert.match(html, /configGet\(MODS_COLLAPSED_KEY\);/);
  assert.match(html, /saveCollapsed: function\(list\)\{ if \(!PREVIEW\) configSet\(MODS_COLLAPSED_KEY, list\); \}/);
  assert.match(html, /\} else if \(d\.key === MODS_COLLAPSED_KEY\) \{\n        if \(d\.result === 1\) \{ try \{ modsUi\.loadCollapsed\(JSON\.parse\(d\.valueJson\)\); \} catch \(e\) \{\} \}/);
  assert.equal(html.split('localStorage').length - 1, 2, 'browser storage stays the preview theme only');
  assert.ok(!/setInterval|setTimeout|requestAnimationFrame/.test(source), 'the tab never polls or defers');
  assert.ok(!/innerHTML/.test(source), 'all text goes through textContent');
  assert.ok(!/[^\x00-\x7f]/.test(source), 'ASCII only');
  /* the scrolling and layout contract */
  assert.match(css, /\.panel-body \{ flex: 1; overflow-y: auto; min-height: 0; \}/);
  assert.ok(/\.mods-row \{[^}]*\}/.test(css) && !/\.mods-row \{[^}]*(position: absolute|height:)/.test(css), 'rows are in flow, no fixed height');
  assert.match(css, /\.mods-toolbar\.panel-toolbar \{ flex-direction: row; align-items: center; \}/);
  assert.match(css, /\.mods-toolbar input\[type=text\] \{ flex: 1 1 auto; width: auto; min-width: 0; \}/);
  assert.match(css, /\.btn\.danger-solid \{ background: var\(--danger\);/);
  assert.ok(!/mods-sizer|mods-shadow|mods-pill/.test(css), 'the windowed list, the shadow list and the pills are gone');
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

console.log('mods_tab_test: OK (' + passed + ' tests)');
