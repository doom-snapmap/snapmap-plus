/* The Mods tab (mods_tab.js): the payload model (one row per mod, Installed or Needs attention, other
   builds counted, duplicate ids), rendering from the fixture payload (tests/fixtures/mods_tab_payload.json,
   whose keys modui_test.c checks against the backend's document), the plain list and its keyboard
   selection, Migrate's visibility, Remove's confirmation, the rescan when the tab is shown, the empty
   and error states, and the page and host contract around it. A minimal DOM stands in for the
   browser. Pure ASCII. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');
const web = path.join(root, 'src', 'ui', 'webview');
const modsTab = require(path.join(web, 'mods_tab.js'));
const fixture = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', 'mods_tab_payload.json'), 'utf8'));
const html = fs.readFileSync(path.join(web, 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const source = fs.readFileSync(path.join(web, 'mods_tab.js'), 'utf8');
const host = fs.readFileSync(path.join(web, 'snapmap_plus_ui_webview.cpp'), 'utf8').replace(/\r\n/g, '\n');
const iface = fs.readFileSync(path.join(root, 'src', 'common', 'snapmap_plus_iface.h'), 'utf8').replace(/\r\n/g, '\n');
const css = fs.readFileSync(path.join(web, 'studio.css'), 'utf8').replace(/\r\n/g, '\n');
let passed = 0;
function test(name, fn) { fn(); passed++; }
const clone = v => JSON.parse(JSON.stringify(v));

/* ---- a minimal DOM ---------------------------------------------------------------------------- */
class FakeNode {
  constructor(tag) {
    this.tagName = tag; this.children = []; this.parentNode = null; this.attrs = {}; this.style = {};
    this.listeners = {}; this.text = ''; this.className = ''; this.scrollTop = 0; this.clientHeight = 0;
    this.disabled = false; this.title = ''; this.value = ''; this.scrolledIntoView = 0;
  }
  get firstChild() { return this.children[0] || null; }
  appendChild(c) { if (c.parentNode) c.parentNode.removeChild(c); c.parentNode = this; this.children.push(c); return c; }
  removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) { this.children.splice(i, 1); c.parentNode = null; } return c; }
  set textContent(v) { this.children.forEach(c => { c.parentNode = null; }); this.children = []; this.text = String(v); }
  get textContent() { return this.text + this.children.map(c => c.textContent).join(' '); }
  set innerHTML(v) { throw new Error('mods_tab.js must not write innerHTML'); }
  setAttribute(k, v) { this.attrs[k] = String(v); }
  getAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attrs, k) ? this.attrs[k] : null; }
  addEventListener(t, f) { (this.listeners[t] = this.listeners[t] || []).push(f); }
  dispatch(t, extra) { const e = Object.assign({type: t, target: this, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }}, extra || {}); (this.listeners[t] || []).forEach(f => f.call(this, e)); return e; }
  click() { if (!this.disabled) this.dispatch('click'); }
  scrollIntoView() { this.scrolledIntoView++; }
  all(cls) {
    const out = [];
    const walk = n => { n.children.forEach(c => { if ((' ' + c.className + ' ').indexOf(' ' + cls + ' ') >= 0) out.push(c); walk(c); }); };
    walk(this);
    return out;
  }
}
const IDS = ['modsList', 'modsStatus', 'modsBadge', 'modsRoot', 'modsDetail', 'modsMap', 'modsFilter', 'modsInstallBtn',
             'modsFolderBtn', 'modsMigrateBtn', 'modsRemoveBtn', 'modsRefreshBtn'];
const GONE = ['modsRescanBtn', 'modsVerifyBtn', 'modsVerifyOneBtn', 'modsPaletteBtn', 'modsPruneBtn', 'modsShadows',
              'modsShadowsSummary', 'modsShadowsBody'];
function harness() {
  const nodes = {};
  IDS.forEach(id => { nodes[id] = new FakeNode('div'); });
  const sent = [], toasts = [], confirms = [];
  const doc = {createElement: tag => new FakeNode(tag)};
  const ui = modsTab.create({
    document: doc, byId: id => nodes[id] || null,
    post: m => sent.push(m), toast: (t, k) => toasts.push({text: t, kind: k}),
    confirm: (t, ok) => confirms.push({text: t, ok: ok})
  });
  ui.bind();
  return {ui, nodes, sent, toasts, confirms};
}
function rows(h) { return h.nodes.modsList.all('mods-row'); }
function rowOf(h, key) { return rows(h).find(n => n.getAttribute('data-key') === key); }
/* As the host answers a rescan: its result, then the document. */
function answer(h, data) {
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', data: {action: 'rescan', ok: true, message: 'Mods folder reread: 7 mod(s) active.'}});
  h.ui.onMessage({kind: 'mods', data: data});
}
function shown(h, data) { h.ui.show(); answer(h, data || fixture); }

/* ---- the model -------------------------------------------------------------------------------- */
test('normalize: the fixture', () => {
  const m = modsTab.normalize(fixture);
  assert.equal(m.ok, true);
  assert.equal(m.error, '');
  assert.equal(m.mods.length, 7);
  assert.deepEqual(m.mods.map(b => b.key), ['local.cyberdemon@0.0.0+1', 'local.cyberdemon@0.0.0-05238b2e+1',
    'pat.<loud>-shotgun@2.1+4', 'local.my-overrides@0.0.0+1', 'sam.quiet-shotgun@1.0.0+1', 'alex.r2-imp@1.0.0+2', 'unbuilt-folder'],
    'sorted by the name shown (name, else id, else folder)');
  assert.deepEqual(m.migrate, {entries: 34, pending: 2, running: false});
  assert.equal(m.rows.length, 2);
  assert.equal(m.placed.length, 2);
  assert.equal(m.entities, 214);
});
test('normalize: hostile and missing values', () => {
  const bad = modsTab.normalize(null);
  assert.equal(bad.ok, false);
  assert.match(bad.error, /unavailable/);
  assert.equal(modsTab.normalize([]).ok, false);
  const m = modsTab.normalize({mods: [{key: 'x', state: 'exploded', size: 'lots', duplicateId: 'yes'}], map: {rows: [{state: '??'}]},
                               migrate: 'soon', storeReadable: false});
  assert.equal(m.mods[0].state, 'attention');
  assert.equal(m.mods[0].size, 0);
  assert.equal(m.mods[0].duplicateId, false);
  assert.equal(m.rows[0].state, 'missing');
  assert.deepEqual(m.migrate, {entries: 0, pending: 0, running: false});
  assert.match(m.error, /cannot be read/);
  assert.equal(modsTab.normalize({}).entities, -2);
  assert.deepEqual(modsTab.normalize({bundles: [{key: 'old'}], shadows: [{request: 'a'}]}).mods, [], 'the retired sections are ignored');
});
test('states: one pill, the cause or the duplicate id as the detail, other builds counted', () => {
  const m = modsTab.normalize(fixture);
  const by = key => m.mods.find(b => b.key === key);
  assert.deepEqual(modsTab.stateInfo(by('alex.r2-imp@1.0.0+2')), {label: 'Installed', tone: 'ok', detail: '1 other build installed'});
  assert.deepEqual(modsTab.stateInfo(by('pat.<loud>-shotgun@2.1+4')), {label: 'Installed', tone: 'ok', detail: ''});
  const quiet = modsTab.stateInfo(by('sam.quiet-shotgun@1.0.0+1'));
  assert.equal(quiet.label, 'Needs attention');
  assert.equal(quiet.detail, 'assets/sound/soundbanks/pc/shotgun.snd is not the size bundle.json records');
  for (const key of ['local.cyberdemon@0.0.0+1', 'local.cyberdemon@0.0.0-05238b2e+1']) {
    const d = modsTab.stateInfo(by(key));
    assert.equal(d.label, 'Needs attention');
    assert.equal(d.detail, 'another installed mod declares this id; change one id in its mod.json');
  }
  assert.equal(modsTab.stateInfo({state: 'installed', duplicateId: true, otherBuilds: 2}).detail,
    'another installed mod declares this id; change one id in its mod.json; 2 other builds installed');
  assert.equal(modsTab.stateInfo({state: 'attention', cause: ''}).detail, 'not in use');
  assert.deepEqual(Object.keys(modsTab.STATES), ['installed', 'attention']);
  assert.ok(!/Legacy|Superseded|Not scanned|Shadow|Conflict/.test(JSON.stringify(modsTab.STATES)), 'no diagnostic states');
  assert.equal(modsTab.filesText(by('local.my-overrides@0.0.0+1')), '63 files, 41 from the installed game');
  assert.equal(modsTab.idText(by('local.cyberdemon@0.0.0-05238b2e+1')), 'local.cyberdemon@0.0.0-05238b2e');
  assert.equal(modsTab.idText(by('unbuilt-folder')), 'unbuilt-folder');
  assert.match(modsTab.rowInfo({state: 'missing', otherKey: 'a@1+2'}).detail, /another build \(a@1\+2\)/);
  assert.equal(modsTab.rowInfo({state: 'missing'}).tone, 'err');
});
test('sizes and filter', () => {
  assert.equal(modsTab.formatSize(0), '0 B');
  assert.equal(modsTab.formatSize(1536), '1.5 KB');
  assert.equal(modsTab.formatSize(1843230), '1.8 MB');
  assert.equal(modsTab.formatSize(3 * 1073741824), '3.0 GB');
  const m = modsTab.normalize(fixture);
  assert.equal(modsTab.filterMods(m.mods, 'CYBER').length, 2);
  assert.equal(modsTab.filterMods(m.mods, 'imp').length, 1);
  assert.equal(modsTab.filterMods(m.mods, '').length, 7);
});
test('map summary', () => {
  const m = modsTab.normalize(fixture);
  const s = modsTab.mapSummary(m);
  assert.equal(s.tone, 'err');
  assert.match(s.text, /^Saving now records 2 mods\. 2 mods new since the map loaded\. 2 recorded mods are no longer placed and will be dropped\. 1 recorded mod is not available on this machine\.$/);
  assert.match(modsTab.mapSummary(Object.assign({}, m, {entities: -1})).text, /Open a map/);
  assert.match(modsTab.mapSummary(Object.assign({}, m, {entities: -2})).text, /cannot read/);
  assert.equal(modsTab.mapSummary(Object.assign({}, m, {rows: [], placed: []})).text, 'This map uses no mods. Saving it records none.');
});
test('action messages', () => {
  assert.deepEqual(modsTab.ACTIONS, ['install', 'remove', 'migrate', 'rescan', 'openFolder']);
  assert.deepEqual(modsTab.actionMessage('rescan'), {cmd: 'modsAction', action: 'rescan'});
  assert.deepEqual(modsTab.actionMessage('remove', 'a@1+2'), {cmd: 'modsAction', action: 'remove', arg: 'a@1+2'});
  for (const gone of ['verify', 'palette', 'pruneList', 'pruneApply', 'format-c']) assert.equal(modsTab.actionMessage(gone), null);
  const t = modsTab.removeConfirmText({key: 'a@1+3', id: 'a', version: '1', name: 'Alpha', otherBuilds: 2});
  assert.match(t, /^Remove Alpha \(a@1\)\?/);
  assert.match(t, /Its 2 other installed builds go too\./);
  assert.match(t, /removed-mods/);
});

/* ---- the controller --------------------------------------------------------------------------- */
test('show rescans once; the list waits for the answer', () => {
  const h = harness();
  assert.equal(h.sent.length, 0, 'nothing is requested until the tab is shown');
  h.ui.show();
  h.ui.show();
  assert.deepEqual(h.sent, [{cmd: 'modsAction', action: 'rescan'}], 'a second show while waiting does not ask again');
  assert.match(h.nodes.modsList.textContent, /Reading installed mods/);
  assert.notEqual(h.nodes.modsStatus.className, 'mods-status busy', 'the quiet rescan does not announce itself');
  answer(h, fixture);
  assert.equal(h.toasts.length, 0, 'a successful rescan on show says nothing');
  h.ui.show();
  assert.equal(h.sent.length, 2, 'showing the tab again rereads the folder');
  assert.equal(h.ui.onMessage({kind: 'other'}), false);
});
test('renders the fixture: one plain list, nothing diagnostic', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsBadge.textContent, '7');
  assert.equal(h.nodes.modsRoot.textContent, fixture.root);
  const r = rows(h);
  assert.equal(r.length, 7, 'every row is mounted');
  assert.ok(r.every(n => n.style.top === undefined), 'rows are in flow, not positioned');
  assert.equal(h.nodes.modsList.children.length, 7, 'no sizer: the rows are the list');
  const imp = rowOf(h, 'alex.r2-imp@1.0.0+2');
  assert.match(imp.textContent, /R2 Imp copy Installed alex\.r2-imp@1\.0\.0 \u00b7 1\.8 MB \u00b7 13 files 1 other build installed/);
  const loud = rowOf(h, 'pat.<loud>-shotgun@2.1+4');
  assert.ok(loud.textContent.indexOf('Loud "shotgun" <b>sounds</b>') >= 0, 'names are text, never markup');
  assert.equal(loud.all('mods-row-detail').length, 0, 'an installed mod with one build has no detail line');
  const cyber = r.filter(n => /local\.cyberdemon/.test(n.getAttribute('data-key')));
  assert.equal(cyber.length, 2);
  cyber.forEach(n => assert.match(n.textContent, /Needs attention .* another installed mod declares this id; change one id in its mod\.json/));
  assert.match(rowOf(h, 'unbuilt-folder').textContent, /^unbuilt-folder Needs attention unbuilt-folder/);
  assert.ok(!/Legacy|Superseded|Not scanned|Shadow|Conflict|Verif/.test(h.nodes.modsList.textContent));
  assert.match(h.nodes.modsStatus.textContent, /^4 mods need attention\.$/);
  assert.match(h.nodes.modsMap.textContent, /Saving now records 2 mods/);
  assert.match(h.nodes.modsMap.textContent, /jo\.lava-floor 1\.2\.0 Missing/);
  assert.match(h.nodes.modsMap.textContent, /Needed by another mod/);
  assert.match(h.nodes.modsDetail.textContent, /Select a mod/);
});
test('Migrate shows only while overrides holds entries the record does not cover', () => {
  const h = harness();
  assert.equal(h.nodes.modsMigrateBtn.style.display, 'none', 'hidden before the first answer');
  shown(h);
  assert.equal(h.nodes.modsMigrateBtn.style.display, '');
  assert.match(h.nodes.modsMigrateBtn.title, /2 entries of the old overrides folder/);
  h.nodes.modsMigrateBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'migrate'});
  assert.match(h.nodes.modsStatus.textContent, /Migrating/);
  assert.equal(h.nodes.modsMigrateBtn.style.display, '', 'stays while its own action runs');
  h.ui.onMessage({kind: 'modsActionResult', action: 'migrate', data: {action: 'migrate', ok: true, message: 'Moved your mods from the old overrides folder into the mods folder.', started: true}});
  assert.deepEqual(h.toasts.pop(), {text: 'Moved your mods from the old overrides folder into the mods folder.', kind: 'ok'});
  const done = clone(fixture);
  done.migrate = {entries: 34, pending: 0, running: false};
  h.ui.onMessage({kind: 'mods', data: done});
  assert.equal(h.nodes.modsMigrateBtn.style.display, 'none');
  done.migrate = {entries: 34, pending: 3, running: true};
  h.ui.onMessage({kind: 'mods', data: done});
  assert.equal(h.nodes.modsMigrateBtn.style.display, 'none', 'not offered while a migration runs');
});
test('selection: click, details, keyboard, Remove with confirmation', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsRemoveBtn.disabled, true);
  rowOf(h, 'alex.r2-imp@1.0.0+2').click();
  assert.equal(h.ui.state().selectedKey, 'alex.r2-imp@1.0.0+2');
  assert.equal(rowOf(h, 'alex.r2-imp@1.0.0+2').getAttribute('aria-selected'), 'true');
  assert.match(h.nodes.modsDetail.textContent, /R2 Imp copy Installed 1 other build installed/);
  assert.match(h.nodes.modsDetail.textContent, /alex\.r2-imp@1\.0\.0/);
  assert.match(h.nodes.modsDetail.textContent, /1\.8 MB, 13 files/);
  assert.equal(h.nodes.modsRemoveBtn.disabled, false);
  /* Arrow keys move the selection and keep it in view; Page keys are left to the browser. */
  let e = h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(e.defaultPrevented, true);
  assert.equal(h.ui.state().selectedKey, 'unbuilt-folder');
  assert.ok(rowOf(h, 'unbuilt-folder').scrolledIntoView > 0);
  h.nodes.modsList.dispatch('keydown', {key: 'Home'});
  assert.equal(h.ui.state().selectedKey, 'local.cyberdemon@0.0.0+1');
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowUp'});
  assert.equal(h.ui.state().selectedKey, 'local.cyberdemon@0.0.0+1');
  h.nodes.modsList.dispatch('keydown', {key: 'End'});
  assert.equal(h.ui.state().selectedKey, 'unbuilt-folder');
  e = h.nodes.modsList.dispatch('keydown', {key: 'PageDown'});
  assert.equal(e.defaultPrevented, false, 'Page Down scrolls natively');
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowUp'});
  assert.equal(h.ui.state().selectedKey, 'alex.r2-imp@1.0.0+2');
  h.nodes.modsRemoveBtn.click();
  assert.equal(h.sent.filter(m => m.action === 'remove').length, 0, 'nothing is removed before the confirmation');
  assert.equal(h.confirms.length, 1);
  assert.match(h.confirms[0].text, /^Remove R2 Imp copy \(alex\.r2-imp@1\.0\.0\)\?\n\nIts 1 other installed build goes too\./);
  h.confirms[0].ok();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'remove', arg: 'alex.r2-imp@1.0.0+2'});
  assert.match(h.nodes.modsStatus.textContent, /Removing/);
  assert.equal(h.nodes.modsRemoveBtn.disabled, true);
  h.ui.onMessage({kind: 'modsActionResult', action: 'remove', data: {action: 'remove', ok: true, message: 'Removed R2 Imp copy.', removed: ['alex.r2-imp@1.0.0+2', 'alex.r2-imp@1.0.0+1']}});
  assert.deepEqual(h.toasts.pop(), {text: 'Removed R2 Imp copy.', kind: 'ok'});
  assert.equal(h.ui.state().selectedKey, '');
  /* A refusal keeps the selection and says why. */
  rowOf(h, 'pat.<loud>-shotgun@2.1+4').click();
  h.nodes.modsRemoveBtn.click();
  h.confirms[1].ok();
  h.ui.onMessage({kind: 'modsActionResult', action: 'remove', data: {action: 'remove', ok: false, message: 'Loud shotgun is used by the open map.', inUse: true, removed: []}});
  assert.equal(h.toasts.pop().kind, 'err');
  assert.equal(h.ui.state().selectedKey, 'pat.<loud>-shotgun@2.1+4');
});
test('a few hundred mods render as one plain list', () => {
  const h = harness();
  const big = clone(fixture);
  big.mods = [];
  for (let i = 0; i < 400; i++)
    big.mods.push(Object.assign(clone(fixture.mods[0]), {key: 'm' + String(i).padStart(3, '0') + '@1+1', name: 'Mod ' + String(i).padStart(3, '0'), otherBuilds: 0}));
  shown(h, big);
  assert.equal(rows(h).length, 400);
  h.nodes.modsFilter.value = 'Mod 39';
  h.nodes.modsFilter.dispatch('input');
  assert.equal(rows(h).length, 10);
});
test('empty and error states', () => {
  const h = harness();
  const empty = clone(fixture);
  empty.mods = []; empty.map = {entities: -1, names: 0, rows: [], placed: []}; empty.migrate = {entries: 0, pending: 0, running: false};
  shown(h, empty);
  assert.match(h.nodes.modsList.textContent, /No mods installed\. Use the install button/);
  assert.match(h.nodes.modsMap.textContent, /Open a map in the editor/);
  assert.equal(h.nodes.modsBadge.textContent, '0');
  assert.equal(h.nodes.modsStatus.textContent, '');
  h.ui.onMessage({kind: 'mods', data: null, error: 'The game did not answer in time. The list is read while the editor is open.'});
  assert.match(h.nodes.modsList.textContent, /did not answer in time/);
  assert.equal(h.ui.state().error.length > 0, true);
  h.ui.onMessage({kind: 'mods', data: Object.assign(clone(fixture), {storeReadable: false})});
  assert.match(h.nodes.modsList.textContent, /cannot be read/);
  h.ui.onMessage({kind: 'mods', data: fixture});
  h.nodes.modsFilter.value = 'zzz';
  h.nodes.modsFilter.dispatch('input');
  assert.match(h.nodes.modsList.textContent, /No installed mod matches/);
  /* A backend without the slot answers the rescan and sends no document. */
  const lone = harness();
  lone.ui.show();
  lone.ui.onMessage({kind: 'modsActionResult', action: 'rescan', ok: false, message: "This build's backend has no Mods tab.", data: null});
  assert.match(lone.nodes.modsList.textContent, /no Mods tab/);
  assert.equal(lone.toasts.length, 0);
  assert.equal(lone.ui.state().waiting, false);
});
test('actions post their messages and hold the buttons while busy', () => {
  const h = harness();
  shown(h);
  h.nodes.modsInstallBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'install'}, 'the host opens the folder picker; the page names no path');
  assert.equal(h.nodes.modsRefreshBtn.disabled, true);
  assert.match(h.nodes.modsStatus.textContent, /Installing/);
  assert.equal(h.ui.act('migrate'), false);
  assert.equal(h.toasts.pop().kind, 'warn');
  h.ui.show();
  assert.equal(h.sent.length, 1, 'showing the tab while an action runs waits for its list');
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', data: {action: 'install', ok: true, message: 'Installed kim.new-crates@0.3.0+1.', key: 'kim.new-crates@0.3.0+1'}});
  assert.equal(h.nodes.modsRefreshBtn.disabled, false);
  assert.deepEqual(h.toasts.pop(), {text: 'Installed kim.new-crates@0.3.0+1.', kind: 'ok'});
  assert.equal(h.ui.state().selectedKey, 'kim.new-crates@0.3.0+1');
  const withNew = clone(fixture);
  withNew.mods.push({key: 'kim.new-crates@0.3.0+1', id: 'kim.new-crates', name: 'New crates', version: '0.3.0', build: 1, size: 120500,
                     files: 5, state: 'installed', cause: '', otherBuilds: 0, duplicateId: false, editorRows: 1, folder: 'kim.new-crates@0.3.0+1'});
  h.ui.onMessage({kind: 'mods', data: withNew});
  assert.match(h.nodes.modsDetail.textContent, /New crates/);
  /* cancelled picker: no toast */
  h.nodes.modsInstallBtn.click();
  const before = h.toasts.length;
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', ok: false, message: '', data: {cancelled: true}});
  assert.equal(h.toasts.length, before);
  assert.equal(h.ui.state().busy, '');
  /* a failed refresh is shown, not toasted */
  h.nodes.modsRefreshBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'rescan'});
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', ok: false, message: 'The game did not run the request in time. Try again with the editor open.', data: null});
  assert.equal(h.nodes.modsStatus.className, 'mods-status err');
  assert.equal(h.toasts.length, before);
  h.nodes.modsFolderBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'openFolder'});
  assert.equal(h.ui.state().busy, '', 'opening the folder does not hold the buttons');
});

/* ---- the page and host contract --------------------------------------------------------------- */
test('page contract', () => {
  assert.ok(html.includes('<div class="tab" data-tab="mods">Mods</div>'));
  assert.ok(html.includes('<div class="content" id="panel-mods" style="display:none;">'));
  IDS.forEach(id => assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id));
  GONE.forEach(id => assert.ok(!html.includes('id="' + id + '"'), 'mockup.html no longer has #' + id));
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<script src="mods_tab.js">'));
  for (const label of ['>Open mods folder<', '>Remove<', '>Migrate<']) assert.ok(panel.includes(label), 'button ' + label);
  assert.ok(panel.includes('id="modsInstallBtn"') && panel.includes('aria-label="Install a mod folder"') && panel.includes('#icon-download'), 'the install button is the download icon');
  assert.ok(!/Verify|Rebuild palette|Remove unused|Shadows and conflicts/.test(panel.slice(0, panel.indexOf('</div>\n  </div>\n') + 1)));
  assert.match(html, /id="modsList" role="listbox" tabindex="0"/, 'the list takes keyboard focus');
  assert.match(html, /id="modsMigrateBtn" style="display:none"/);
  assert.equal(html.split('<script src="mods_tab.js"></script>').length, 2, 'one module tag');
  assert.ok(html.indexOf('<script src="mods_tab.js"></script>') > html.indexOf('<script src="prefab_budget.js"></script>'));
  assert.match(html, /function onMessage\(d\) \{\n    if \(typeof modsUi !== 'undefined' && modsUi\.onMessage\(d\)\) return;/);
  assert.match(html, /\['entities','prefabs','timelines','assets','mods'\]/);
  assert.match(html, /if \(which === 'mods'\) modsUi\.show\(\);/);
  assert.ok(html.includes("var PREVIEW_MODS = {v:2,"), 'browser preview answers with the current shape');
  assert.ok(!/setInterval|setTimeout|requestAnimationFrame/.test(source), 'the tab never polls or defers');
  assert.ok(!/innerHTML/.test(source), 'all text goes through textContent');
  assert.ok(!/[^\x00-\x7f]/.test(source), 'ASCII only');
  /* The scrolling contract: the list is a bounded flex child that scrolls; its rows are in flow. */
  assert.match(css, /\.panel-body \{ flex: 1; overflow-y: auto; min-height: 0; \}/);
  assert.ok(/\.mods-row \{[^}]*\}/.test(css) && !/\.mods-row \{[^}]*(position: absolute|height:)/.test(css), 'rows are in flow, no fixed height');
  assert.ok(!/mods-sizer|mods-shadow/.test(css), 'the windowed list and the shadow list are gone');
});
test('host and interface contract', () => {
  assert.match(iface, /sh_mods_fn\s+mods;\s+\/\* \+0x378 \(ext 34\) Mods tab \*\//);
  assert.match(iface, /SH_STATIC_ASSERT\(offsetof\(sh_iface_vtbl, mods\) == 0x378\);/);
  assert.match(iface, /SH_STATIC_ASSERT\(sizeof\(sh_iface_vtbl\) == 0x380\);/);
  assert.match(iface, /#define SH_MODS_WAIT  5/);
  assert.match(iface, /#define SH_MODS_ACT_REMOVE      7\n#define SH_MODS_ACT_MIGRATE     8/);
  assert.ok(host.includes('} else if (cmd == L"modsAction") {'));
  assert.match(host, /else if \(code == SH_MODS_ACT_INSTALL\) poc_mods_begin\(code, true, std::string\(\)\);/, 'install takes its folder from the native picker only');
  assert.ok(host.includes('L"{\\"kind\\":\\"mods\\",\\"data\\":"'));
  assert.ok(host.includes('{\\"kind\\":\\"modsActionResult\\",\\"action\\":\\"'));
  assert.ok(host.includes('SH_IFACE_HAS_SLOT(g_iface, mods)'), 'an older backend is never called past its table');
  assert.ok(host.includes('FOS_PICKFOLDERS'));
  assert.match(host, /if \(job\.action == SH_MODS_ACT_MIGRATE && job\.run_rc == 1\) \{[\s\S]*?wait\.op = SH_MODS_WAIT;/, 'Migrate follows the worker before the list');
  assert.ok(!/ShellExecute|CreateProcess/.test(host.slice(host.indexOf('/* The Mods tab'), host.indexOf('/* Extract flat sidecar tags'))));
  for (const a of modsTab.ACTIONS.filter(a => a !== 'openFolder'))
    assert.ok(host.includes('if (name == L"' + a + '")'), 'host maps ' + a);
});

console.log('mods_tab_test: OK (' + passed + ' tests)');
