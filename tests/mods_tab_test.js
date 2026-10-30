/* The Mods tab (mods_tab.js): the payload model, state and shadow explanations, the windowed lists,
   rendering from the fixture payload (tests/fixtures/mods_tab_payload.json, whose keys modui_test.c
   checks against the backend's document), the action messages, the empty and error states, and the
   page and host contract around it. A minimal DOM stands in for the browser. Pure ASCII. */
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
const iface = fs.readFileSync(path.join(root, 'src', 'common', 'snapmap_plus_iface.h'), 'utf8');
const css = fs.readFileSync(path.join(web, 'studio.css'), 'utf8');
let passed = 0;
function test(name, fn) { fn(); passed++; }
const clone = v => JSON.parse(JSON.stringify(v));

/* ---- a minimal DOM ---------------------------------------------------------------------------- */
class FakeNode {
  constructor(tag) {
    this.tagName = tag; this.children = []; this.parentNode = null; this.attrs = {}; this.style = {};
    this.listeners = {}; this.text = ''; this.className = ''; this.scrollTop = 0; this.clientHeight = 0;
    this.disabled = false; this.title = ''; this.value = ''; this.open = false;
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
  dispatch(t) { (this.listeners[t] || []).forEach(f => f.call(this, {type: t, target: this})); }
  click() { if (!this.disabled) this.dispatch('click'); }
  all(cls) {
    const out = [];
    const walk = n => { n.children.forEach(c => { if ((' ' + c.className + ' ').indexOf(' ' + cls + ' ') >= 0) out.push(c); walk(c); }); };
    walk(this);
    return out;
  }
}
const IDS = ['modsList', 'modsStatus', 'modsBadge', 'modsRoot', 'modsDetail', 'modsMap', 'modsShadows', 'modsShadowsSummary',
             'modsShadowsBody', 'modsFilter', 'modsInstallBtn', 'modsRescanBtn', 'modsVerifyBtn', 'modsVerifyOneBtn',
             'modsPaletteBtn', 'modsPruneBtn', 'modsFolderBtn', 'modsRefreshBtn'];
function harness() {
  const nodes = {};
  IDS.forEach(id => { nodes[id] = new FakeNode('div'); });
  nodes.modsList.clientHeight = 200;
  nodes.modsShadowsBody.clientHeight = 152;
  const sent = [], toasts = [], confirms = [];
  const doc = {createElement: tag => new FakeNode(tag)};
  const ui = modsTab.create({
    document: doc, byId: id => nodes[id] || null,
    post: m => sent.push(m), toast: (t, k) => toasts.push({text: t, kind: k}),
    confirm: (t, ok) => confirms.push({text: t, ok: ok}), raf: fn => fn()
  });
  ui.bind();
  return {ui, nodes, sent, toasts, confirms};
}
function rows(h) { return h.nodes.modsList.all('mods-row'); }

/* ---- the model -------------------------------------------------------------------------------- */
test('normalize: the fixture', () => {
  const m = modsTab.normalize(fixture);
  assert.equal(m.ok, true);
  assert.equal(m.error, '');
  assert.equal(m.bundles.length, 6);
  assert.deepEqual(m.bundles.map(b => b.state), ['active', 'legacy', 'unindexed', 'skipped', 'active', 'superseded'],
                   'sorted by name, the newest build of one mod first');
  assert.equal(m.bundles[4].key, 'alex.r2-imp@1.0.0+2');
  assert.equal(m.rows.length, 2);
  assert.equal(m.placed.length, 2);
  assert.equal(m.shadows.length, 4);
  assert.deepEqual(m.totals, {stock: 1, legacy: 62, conflict: 1, total: 64, sent: 4});
  assert.equal(m.entities, 214);
});
test('normalize: hostile and missing values', () => {
  const bad = modsTab.normalize(null);
  assert.equal(bad.ok, false);
  assert.match(bad.error, /unavailable/);
  assert.equal(modsTab.normalize([]).ok, false);
  const m = modsTab.normalize({bundles: [{key: 'x', state: 'exploded', size: 'lots', names: 7}], map: {rows: [{state: '??'}]},
                               shadows: [{kind: 'weird', bundles: 'nope'}], storeReadable: false});
  assert.equal(m.bundles[0].state, 'skipped');
  assert.equal(m.bundles[0].size, 0);
  assert.equal(m.rows[0].state, 'missing');
  assert.equal(m.shadows[0].kind, 'stock');
  assert.deepEqual(m.shadows[0].bundles, []);
  assert.match(m.error, /cannot be read/);
  assert.equal(modsTab.normalize({}).entities, -2);
});
test('state and shadow explanations', () => {
  const m = modsTab.normalize(fixture);
  const by = key => m.bundles.find(b => b.key === key);
  assert.equal(modsTab.stateInfo(by('alex.r2-imp@1.0.0+2')).label, 'Active');
  assert.match(modsTab.stateInfo(by('alex.r2-imp@1.0.0+1')).detail, /newer build \(alex\.r2-imp@1\.0\.0\+2\) serves 1 of its files/);
  assert.equal(modsTab.stateInfo(by('sam.quiet-shotgun@1.0.0+1')).tone, 'err');
  assert.match(modsTab.stateInfo(by('sam.quiet-shotgun@1.0.0+1')).detail, /^Not served: assets\/sound/);
  assert.match(modsTab.stateInfo(by('kim.new-crates@0.3.0+1')).detail, /Rescan/);
  assert.equal(modsTab.stateInfo(by('local.my-overrides@0.0.0+1')).label, 'Legacy');
  assert.match(modsTab.stateInfo({state: 'active', cause: 'x is missing; Verify to check the build'}).detail, /Warning: x is missing/);
  assert.equal(modsTab.shadowInfo({kind: 'stock'}).text, 'Applies on this machine only.');
  assert.equal(modsTab.shadowInfo({kind: 'legacy'}).text, 'Changes what maps mean.');
  assert.equal(modsTab.shadowInfo({kind: 'conflict'}).text, 'Two mods supply this name; neither is served.');
  assert.equal(modsTab.shadowInfo({kind: 'conflict', resolution: 'chosen', served: 'local.cyberdemon@0.0.0+1',
    reason: 'most providers carry these bytes'}).text,
    'Several mods supply this name; local.cyberdemon@0.0.0+1 serves it (most providers carry these bytes).');
  assert.match(modsTab.shadowInfo({kind: 'conflict', resolution: 'composed'}).text, /entries of all of them are served/);
  assert.match(modsTab.shadowInfo({kind: 'conflict', resolution: 'withdrawn'}).text, /game copy is used/);
  assert.equal(modsTab.shadowInfo({kind: 'conflict', resolution: 'chosen'}).tone, 'warn');
  /* Stock pointers: files served from the installed game's own copy. */
  assert.equal(modsTab.filesText(by('local.my-overrides@0.0.0+1')), '63 files, 41 from the installed game');
  assert.equal(modsTab.filesText(by('alex.r2-imp@1.0.0+2')), '13 files');
  assert.ok(m.shadows.find(s => s.request === 'generated/decls/snapeditorentitydef/demons/imp.decl').installed);
  assert.match(modsTab.shadowInfo({kind: 'legacy', installed: true}).text, /^Changes what maps mean\. Served from the installed game's own copy\.$/);
  assert.equal(modsTab.shadowInfo({kind: 'stock', installed: false}).text, 'Applies on this machine only.');
  assert.equal(modsTab.normalize({ok: true, bundles: [], shadows: [{request: 'a', kind: 'conflict', resolution: 'composed',
    reason: 'composed: 2 list entries'}]}).shadows[0].resolution, 'composed');
  assert.match(modsTab.rowInfo({state: 'missing', otherKey: 'a@1+2'}).detail, /another build \(a@1\+2\)/);
  assert.equal(modsTab.rowInfo({state: 'missing'}).tone, 'err');
});
test('sizes, filter, window', () => {
  assert.equal(modsTab.formatSize(0), '0 B');
  assert.equal(modsTab.formatSize(1536), '1.5 KB');
  assert.equal(modsTab.formatSize(1843230), '1.8 MB');
  assert.equal(modsTab.formatSize(3 * 1073741824), '3.0 GB');
  const m = modsTab.normalize(fixture);
  assert.equal(modsTab.filterBundles(m.bundles, 'IMP').length, 2);
  assert.equal(modsTab.filterBundles(m.bundles, 'skipped').length, 1);
  assert.equal(modsTab.filterBundles(m.bundles, '').length, 6);
  assert.deepEqual(modsTab.windowRange(0, 200, 40, 0, 6), {start: 0, end: 0});
  assert.deepEqual(modsTab.windowRange(0, 200, 40, 5000, 6), {start: 0, end: 11});
  assert.deepEqual(modsTab.windowRange(40000, 200, 40, 5000, 6), {start: 994, end: 1011});
  assert.deepEqual(modsTab.windowRange(1e9, 200, 40, 5000, 6), {start: 5000, end: 5000});
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
  assert.deepEqual(modsTab.actionMessage('rescan'), {cmd: 'modsAction', action: 'rescan'});
  assert.deepEqual(modsTab.actionMessage('verify', 'a@1+2'), {cmd: 'modsAction', action: 'verify', arg: 'a@1+2'});
  assert.equal(modsTab.actionMessage('format-c'), null);
  const t = modsTab.pruneConfirmText([{key: 'a@1+1', size: 2048, coveredBy: 'a@1+2'}]);
  assert.match(t, /^Remove 1 build that a newer build/);
  assert.match(t, /a@1\+1 \(2\.0 KB\), covered by a@1\+2/);
  assert.match(t, /cannot be undone/);
});

/* ---- the controller --------------------------------------------------------------------------- */
test('show asks once; the list waits for the answer', () => {
  const h = harness();
  assert.equal(h.sent.length, 0, 'nothing is requested until the tab is shown');
  h.ui.show();
  h.ui.show();
  assert.deepEqual(h.sent, [{cmd: 'modsList'}], 'a second show while waiting does not ask again');
  assert.match(h.nodes.modsList.textContent, /Reading installed mods/);
  h.ui.onMessage({kind: 'mods', data: fixture});
  h.ui.show();
  assert.equal(h.sent.length, 2, 'showing the tab again reads the state again');
  assert.equal(h.ui.onMessage({kind: 'other'}), false);
});
test('renders the fixture', () => {
  const h = harness();
  h.ui.onMessage({kind: 'mods', data: fixture});
  assert.equal(h.nodes.modsBadge.textContent, '6');
  assert.equal(h.nodes.modsRoot.textContent, fixture.root);
  const r = rows(h);
  assert.equal(r.length, 6);
  assert.match(r[1].textContent, /My overrides/);
  assert.match(r[1].textContent, /Legacy/);
  const loud = r.find(n => n.getAttribute('data-key') === 'pat.<loud>-shotgun@2.1+4');
  assert.ok(loud.textContent.indexOf('Loud "shotgun" <b>sounds</b>') >= 0, 'names are text, never markup');
  assert.equal(r[5].style.top, '200px');
  assert.match(h.nodes.modsStatus.textContent, /4 mods serving, 92 files/);
  assert.match(h.nodes.modsMap.textContent, /Saving now records 2 mods/);
  assert.match(h.nodes.modsMap.textContent, /jo\.lava-floor 1\.2\.0 Missing/);
  assert.match(h.nodes.modsMap.textContent, /Needed by another mod/);
  assert.match(h.nodes.modsMap.textContent, /New/);
  assert.match(h.nodes.modsShadowsSummary.textContent, /1 shadow, 62 legacy shadows, 1 conflict/);
  assert.equal(h.nodes.modsShadowsBody.children.length, 0, 'the shadow list is not built until opened');
  assert.match(h.nodes.modsDetail.textContent, /Select a mod/);
});
test('selection shows the details and enables Verify', () => {
  const h = harness();
  h.ui.onMessage({kind: 'mods', data: fixture});
  assert.equal(h.nodes.modsVerifyOneBtn.disabled, true);
  rows(h).find(n => n.getAttribute('data-key') === 'alex.r2-imp@1.0.0+1').click();
  assert.equal(h.ui.state().selectedKey, 'alex.r2-imp@1.0.0+1');
  assert.match(h.nodes.modsDetail.textContent, /Superseded/);
  assert.match(h.nodes.modsDetail.textContent, /1\.0\.0 build 1/);
  assert.match(h.nodes.modsDetail.textContent, /1\.8 MB, 13 files/);
  assert.equal(h.nodes.modsVerifyOneBtn.disabled, false);
  h.nodes.modsVerifyOneBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'verify', arg: 'alex.r2-imp@1.0.0+1'});
  assert.equal(rows(h).find(n => n.getAttribute('data-key') === 'alex.r2-imp@1.0.0+1').getAttribute('aria-selected'), 'true');
});
test('shadows open lazily with their explanations', () => {
  const h = harness();
  h.ui.onMessage({kind: 'mods', data: fixture});
  h.nodes.modsShadows.open = true;
  h.nodes.modsShadows.dispatch('toggle');
  const s = h.nodes.modsShadowsBody.all('mods-shadow-row');
  assert.equal(s.length, 4);
  assert.match(s[2].textContent, /Conflict sound\/soundbanks\/pc\/shotgun\.snd/);
  assert.match(s[2].textContent, /Several mods supply this name; tom\.shotgun@1\.0\.0\+1 serves it \(installed most recently\)\. From pat\.<loud>-shotgun@2\.1\+4, tom\.shotgun@1\.0\.0\+1\./);
  assert.match(s[0].textContent, /Changes what maps mean\. By local\.my-overrides/);
  assert.match(s[3].textContent, /Applies on this machine only\./);
  assert.match(h.nodes.modsShadowsBody.textContent, /60 more not listed/);
  h.nodes.modsShadows.open = false;
  h.nodes.modsShadows.dispatch('toggle');
  assert.equal(h.nodes.modsShadowsBody.children.length, 0);
});
test('a large store mounts only the rows in view', () => {
  const h = harness();
  const big = clone(fixture);
  big.bundles = [];
  for (let i = 0; i < 5000; i++)
    big.bundles.push(Object.assign(clone(fixture.bundles[1]), {key: 'm' + String(i).padStart(5, '0') + '@1+1', name: 'Mod ' + String(i).padStart(5, '0')}));
  big.shadows = [];
  for (let i = 0; i < 4000; i++) big.shadows.push({request: 'p/' + i, kind: 'stock', shadow: true, served: 'x', bundles: ['x']});
  h.ui.onMessage({kind: 'mods', data: big});
  let r = rows(h);
  assert.ok(r.length <= 11, 'mounted ' + r.length);
  assert.equal(h.nodes.modsList.firstChild.style.height, (5000 * modsTab.ROW_HEIGHT) + 'px');
  h.nodes.modsList.scrollTop = 1000 * modsTab.ROW_HEIGHT;
  h.nodes.modsList.dispatch('scroll');
  r = rows(h);
  assert.ok(r.length <= 17, 'mounted ' + r.length);
  assert.ok(r.some(n => n.getAttribute('data-key') === 'm01000@1+1'));
  assert.ok(!r.some(n => n.getAttribute('data-key') === 'm00000@1+1'));
  h.nodes.modsShadows.open = true;
  h.nodes.modsShadows.dispatch('toggle');
  assert.ok(h.nodes.modsShadowsBody.all('mods-shadow-row').length <= 11);
  h.nodes.modsFilter.value = 'Mod 0499';
  h.nodes.modsFilter.dispatch('input');
  assert.equal(rows(h).length, 10);
});
test('empty and error states', () => {
  const h = harness();
  const empty = clone(fixture);
  empty.bundles = []; empty.shadows = []; empty.map = {entities: -1, names: 0, rows: [], placed: []};
  empty.shadowTotals = {stock: 0, legacy: 0, conflict: 0, total: 0, sent: 0};
  h.ui.onMessage({kind: 'mods', data: empty});
  assert.match(h.nodes.modsList.textContent, /No mods installed\. Use Install/);
  assert.match(h.nodes.modsMap.textContent, /Open a map in the editor/);
  assert.equal(h.nodes.modsBadge.textContent, '0');
  h.nodes.modsShadows.open = true;
  h.nodes.modsShadows.dispatch('toggle');
  assert.match(h.nodes.modsShadowsBody.textContent, /No installed mod replaces/);
  h.ui.onMessage({kind: 'mods', data: null, error: 'The game did not answer in time. The list is read while the editor is open.'});
  assert.match(h.nodes.modsList.textContent, /did not answer in time/);
  assert.equal(h.ui.state().error.length > 0, true);
  h.ui.onMessage({kind: 'mods', data: Object.assign(clone(fixture), {storeReadable: false})});
  assert.match(h.nodes.modsList.textContent, /cannot be read/);
  h.ui.onMessage({kind: 'mods', data: fixture});
  h.nodes.modsFilter.value = 'zzz';
  h.nodes.modsFilter.dispatch('input');
  assert.match(h.nodes.modsList.textContent, /No installed mod matches/);
});
test('actions post their messages and hold the buttons while busy', () => {
  const h = harness();
  h.ui.onMessage({kind: 'mods', data: fixture});
  h.nodes.modsInstallBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'install'}, 'the host opens the folder picker; the page names no path');
  assert.equal(h.nodes.modsRescanBtn.disabled, true);
  assert.match(h.nodes.modsStatus.textContent, /Installing/);
  assert.equal(h.ui.act('rescan'), false);
  assert.equal(h.toasts.pop().kind, 'warn');
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', data: {action: 'install', ok: true, message: 'Installed kim.new-crates@0.3.0+1.', key: 'kim.new-crates@0.3.0+1'}});
  assert.equal(h.nodes.modsRescanBtn.disabled, false);
  assert.deepEqual(h.toasts.pop(), {text: 'Installed kim.new-crates@0.3.0+1.', kind: 'ok'});
  assert.equal(h.ui.state().selectedKey, 'kim.new-crates@0.3.0+1');
  h.ui.onMessage({kind: 'mods', data: fixture});
  assert.match(h.nodes.modsDetail.textContent, /New crates/);
  /* cancelled picker: no toast */
  h.nodes.modsInstallBtn.click();
  const before = h.toasts.length;
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', ok: false, message: '', data: {cancelled: true}});
  assert.equal(h.toasts.length, before);
  assert.equal(h.ui.state().busy, '');
  [['modsRescanBtn', 'rescan'], ['modsVerifyBtn', 'verify'], ['modsPaletteBtn', 'palette']].forEach(([id, action]) => {
    h.nodes[id].click();
    assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: action});
    h.ui.onMessage({kind: 'modsActionResult', action: action, ok: false, message: 'The game did not run the request in time. Try again with the editor open.', data: null});
    assert.equal(h.toasts.pop().kind, 'err');
    assert.equal(h.nodes.modsStatus.className, 'mods-status err');
  });
  h.nodes.modsFolderBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'openFolder'});
  assert.equal(h.ui.state().busy, '', 'opening the folder does not hold the buttons');
  h.nodes.modsRefreshBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsList'});
});
test('remove unused: list, confirm, then remove exactly the listed keys', () => {
  const h = harness();
  h.ui.onMessage({kind: 'mods', data: fixture});
  h.nodes.modsPruneBtn.click();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'pruneList'});
  h.ui.onMessage({kind: 'modsActionResult', action: 'pruneList', data: {action: 'pruneList', ok: true, message: 'No build can be removed: ...', candidates: []}});
  assert.equal(h.confirms.length, 0, 'nothing to confirm');
  assert.match(h.toasts.pop().text, /No build can be removed/);
  h.nodes.modsPruneBtn.click();
  h.sent.pop();
  h.ui.onMessage({kind: 'modsActionResult', action: 'pruneList', data: {action: 'pruneList', ok: true, message: '2 build(s) can be removed.',
    candidates: [{key: 'alex.r2-imp@1.0.0+1', size: 1843202, coveredBy: 'alex.r2-imp@1.0.0+2'}, {key: 'b@1+1', size: 10, coveredBy: 'b@1+3'}]}});
  assert.equal(h.sent.filter(m => m.action === 'pruneApply').length, 0, 'nothing is removed before the confirmation');
  assert.equal(h.confirms.length, 1);
  assert.match(h.confirms[0].text, /Remove 2 builds/);
  h.confirms[0].ok();
  assert.deepEqual(h.sent.pop(), {cmd: 'modsAction', action: 'pruneApply', arg: 'alex.r2-imp@1.0.0+1\nb@1+1'});
  h.ui.onMessage({kind: 'modsActionResult', action: 'pruneApply', data: {action: 'pruneApply', ok: true, message: 'Removed 2 build(s).', removed: ['alex.r2-imp@1.0.0+1', 'b@1+1']}});
  assert.deepEqual(h.toasts.pop(), {text: 'Removed 2 build(s).', kind: 'ok'});
});

/* ---- the page and host contract --------------------------------------------------------------- */
test('page contract', () => {
  assert.ok(html.includes('<div class="tab" data-tab="mods">Mods</div>'));
  assert.ok(html.includes('<div class="content" id="panel-mods" style="display:none;">'));
  IDS.forEach(id => assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id));
  assert.equal(html.split('<script src="mods_tab.js"></script>').length, 2, 'one module tag');
  assert.ok(html.indexOf('<script src="mods_tab.js"></script>') > html.indexOf('<script src="prefab_budget.js"></script>'));
  assert.match(html, /function onMessage\(d\) \{\n    if \(typeof modsUi !== 'undefined' && modsUi\.onMessage\(d\)\) return;/);
  assert.match(html, /\['entities','prefabs','timelines','assets','mods'\]/);
  assert.match(html, /if \(which === 'mods'\) modsUi\.show\(\);/);
  assert.ok(html.includes("if (o.cmd === 'modsList') { onMessage({kind:'mods', data: PREVIEW_MODS}); return; }"), 'browser preview answers');
  assert.ok(!/setInterval|setTimeout/.test(source), 'the tab never polls');
  assert.ok(!/innerHTML/.test(source), 'all text goes through textContent');
  assert.ok(!/[^\x00-\x7f]/.test(source), 'ASCII only');
  assert.ok(/\.mods-row \{[^}]*height: 40px/.test(css) && modsTab.ROW_HEIGHT === 40, 'row height matches the stylesheet');
  assert.ok(/\.mods-shadow-row \{[^}]*height: 38px/.test(css) && modsTab.SHADOW_ROW_HEIGHT === 38);
});
test('host and interface contract', () => {
  assert.match(iface, /sh_mods_fn\s+mods;\s+\/\* \+0x378 \(ext 34\) Mods tab \*\//);
  assert.match(iface, /SH_STATIC_ASSERT\(offsetof\(sh_iface_vtbl, mods\) == 0x378\);/);
  assert.match(iface, /SH_STATIC_ASSERT\(sizeof\(sh_iface_vtbl\) == 0x380\);/);
  assert.ok(host.includes('} else if (cmd == L"modsList") {'));
  assert.ok(host.includes('} else if (cmd == L"modsAction") {'));
  assert.match(host, /else if \(code == SH_MODS_ACT_INSTALL\) poc_mods_begin\(code, true, std::string\(\)\);/, 'install takes its folder from the native picker only');
  assert.ok(host.includes('L"{\\"kind\\":\\"mods\\",\\"data\\":"'));
  assert.ok(host.includes('{\\"kind\\":\\"modsActionResult\\",\\"action\\":\\"'));
  assert.ok(host.includes('SH_IFACE_HAS_SLOT(g_iface, mods)'), 'an older backend is never called past its table');
  assert.ok(host.includes('FOS_PICKFOLDERS'));
  assert.ok(!/ShellExecute|CreateProcess/.test(host.slice(host.indexOf('/* The Mods tab'), host.indexOf('/* Extract flat sidecar tags'))));
  for (const a of modsTab.ACTIONS.filter(a => a !== 'openFolder'))
    assert.ok(host.includes('if (name == L"' + a + '")'), 'host maps ' + a);
});

console.log('mods_tab_test: OK (' + passed + ' tests)');
