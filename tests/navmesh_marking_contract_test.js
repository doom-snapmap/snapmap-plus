/* Navigation section: the AI-navigation marking contract, asserted against mockup.html's source and
 * by running the section's own logic out of it. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const HTML = path.join(root, 'src', 'ui', 'webview', 'mockup.html');
const HOST = path.join(root, 'src', 'ui', 'webview', 'snapmap_plus_ui_webview.cpp');
const IFACE = path.join(root, 'src', 'common', 'snapmap_plus_iface.h');
const src = fs.readFileSync(HTML, 'utf8').replace(/\r\n/g, '\n');
const hostSrc = fs.readFileSync(HOST, 'utf8').replace(/\r\n/g, '\n');
const ifaceSrc = fs.readFileSync(IFACE, 'utf8').replace(/\r\n/g, '\n');

let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}
function slice(from, to, what) {
  const a = src.indexOf(from);
  const b = src.indexOf(to, a);
  if (a < 0 || b < 0) throw new Error('could not extract ' + what + ' from mockup.html');
  return src.slice(a, b);
}

/* ---- the section exists and is registered the way the other sections are -------------------- */

check(src.indexOf('<div class="tab" data-tab="navigation">Navigation</div>') >= 0,
  'the tabstrip has no Navigation tab');
check(src.indexOf('id="panel-navigation"') >= 0, 'there is no #panel-navigation content panel');
check(src.indexOf("['entities','prefabs','timelines','assets','navigation'].forEach") >= 0,
  'the Navigation panel is not registered in the tab-switch show/hide list');
check(src.indexOf("if (which === 'navigation') navOnShow();") >= 0,
  'switching to Navigation does not tell the section to populate itself');
check(/<div class="tabstrip">[\s\S]*data-tab="navigation"[\s\S]*<\/div>\s*\n\s*<div class="content"/.test(src),
  'the Navigation tab is not inside the shared tabstrip');

const panel = slice('<div class="content" id="panel-navigation"', '<div class="statusbar">', 'the Navigation panel');
check(panel.indexOf('class="panel-head"') >= 0 && panel.indexOf('class="panel-title">Navigation') >= 0 &&
      panel.indexOf('class="panel-toolbar"') >= 0 && panel.indexOf('class="panel-body list"') >= 0,
  'the Navigation panel does not reuse the shared panel skeleton');
check(panel.indexOf('id="navBadge"') >= 0 && panel.indexOf('class="badge"') >= 0,
  'the Navigation panel has no count badge like the other list panels');

/* Iconography: reuse only, no new SVG. Every icon the panel references must already be declared. */
const usedIcons = (panel.match(/href="#(icon-[a-z0-9-]+)"/g) || []).map(function (m) { return m.slice(7, -1); });
check(usedIcons.length > 0, 'the Navigation panel uses no shared icon at all');
usedIcons.forEach(function (id) {
  check(src.indexOf('<symbol id="' + id + '"') >= 0,
    'Navigation references ' + id + ', which is not in the embedded Lucide subset');
});

/* ---- no new ABI: the section rides existing bridge commands and existing vtable slots -------- */

const nav = slice("var NAV_ENABLED_KEY = 'navmesh.enabled';", '// ---- messages from native ----',
                  'the Navigation module');
const posted = (nav.match(/post\(\{cmd: *'([a-zA-Z]+)'/g) || [])
  .map(function (m) { return m.replace(/^post\(\{cmd: *'/, '').replace(/'$/, ''); });
check(posted.length > 0, 'the Navigation module never talks to the bridge');
posted.forEach(function (cmd) {
  check(cmd === 'select' || cmd === 'save',
    'Navigation posts "' + cmd + '"; it may only reuse the existing select/save round-trips');
  check(hostSrc.indexOf('cmd == L"' + cmd + '"') >= 0,
    'Navigation posts "' + cmd + '", which the native host does not already handle');
});
check(hostSrc.indexOf('navmesh') < 0 && hostSrc.indexOf('affectsNavmesh') < 0,
  'the WebView host grew a navigation-specific command; the section must add no bridge surface');
check(ifaceSrc.indexOf('affectsNavmesh') < 0,
  'the interface ABI grew an affectsNavmesh slot; the section must add no interface slot');

/* ---- the exact strings the feature is defined by --------------------------------------------- */

check(/var NAV_ENABLED_KEY = 'navmesh\.enabled';/.test(src),
  "the master switch's config key is not exactly 'navmesh.enabled'");
check(/var NAV_BLOCKING_INHERIT = 'snapmaps\/volume\/blocking';/.test(src),
  "the Blocking Box inherit is not exactly 'snapmaps/volume/blocking'");
check(/var NAV_FLAG_KEY = 'affectsNavmesh';/.test(src),
  "the edit-state leaf is not exactly 'affectsNavmesh'");
check(nav.indexOf('d.inherit === NAV_BLOCKING_INHERIT') >= 0,
  'a row is not selected by an exact inherit match');
check(nav.indexOf("dpSetScalar(e.text, e.open, e.close, NAV_FLAG_KEY, row.marked ? 'true' : 'false')") >= 0,
  'the toggle does not patch the decl-source edit block to a bare true/false through dpSetScalar');
check(nav.indexOf('dpEditBlock(row.decl') >= 0,
  'the toggle does not patch the decl source the read round-trip returned');
check(/var NAV_SIZE_BLOCK = 'clipModelInfo';/.test(src) && /var NAV_SIZE_KEY = 'size';/.test(src),
  'the size read is not clipModelInfo.size');

/* ---- the config key uses the existing persistence mechanism, unchanged ----------------------- */

check(src.indexOf('configGet(NAV_ENABLED_KEY);') >= 0,
  'the master switch is never hydrated from the backend config service at startup');
check(/configGet\('theme'\);[\s\S]{0,240}configGet\(NAV_ENABLED_KEY\);/.test(src),
  'the master switch is not requested in the same startup block as the other settings');
check(nav.indexOf('if (!PREVIEW) configSet(NAV_ENABLED_KEY, navEnabled);') >= 0,
  'the master switch is not written back through the shared configSet bridge');
check(src.indexOf('d.key === NAV_ENABLED_KEY') >= 0,
  'a rejected navmesh.enabled write is not reported like the other settings');
check(src.indexOf('!navReceiveSetting(d)') >= 0,
  'the configValue router does not offer navmesh.enabled to the Navigation section');
check(nav.indexOf('localStorage') < 0,
  'the Navigation section invented its own storage instead of the config service');

/* ---- the commit rides the one Save-to-Decl path --------------------------------------------- */

check(nav.indexOf('navConsumeSaveResult') >= 0 &&
      src.indexOf('if (navConsumeSaveResult(d)) return;') >= 0,
  'the shared saveResult is not routed to whichever surface issued the commit');
check(nav.indexOf("toast('Save or revert your Entity State edits for this entity first.', 'warn')") >= 0,
  'toggling can commit a cached decl over an unsaved Entity State edit');
check(src.indexOf('if (navScanReply && isDirty()) return;') >= 0,
  "a Navigation read can overwrite the Entity State panel's edit in progress");

/* ---- preview mode still self-populates ------------------------------------------------------- */

check(/\{eid:66,[^}]*name:'Arena Floor'/.test(src) && /\{eid:68,[^}]*name:'Cover Crate'/.test(src),
  'browser preview has no sample blocking volumes for the Navigation list');
check((src.match(/inherit:'snapmaps\/volume\/blocking'/g) || []).length >= 4,
  'browser preview does not carry enough sample Blocking Boxes to exercise the list');
check(src.indexOf('affectsNavmesh = true;') >= 0,
  'no preview volume is marked, so the marked state cannot be seen in a plain browser');

/* ---- run the section's own logic ------------------------------------------------------------- */

const dp = slice('function dpMatchBrace(s, open)', 'function dpApply(decl, type, carrier, name, rect, extra)',
                 'the decl-text patch helpers');

function node() {
  return {value: '', checked: false, textContent: '', innerHTML: '', scrollTop: 0,
          style: {display: 'flex'}, addEventListener: function () {}};
}
const nodes = {
  navList: node(), navFilter: node(), navBadge: node(), navSummary: node(),
  navEnabled: node(), navRefreshBtn: node()
};
nodes['panel-navigation'] = node();
nodes.navEnabled.checked = true;
const document = {getElementById: function (id) { return nodes[id] || null; }};

const STATES = {
  10: {inherit: 'snapmaps/volume/blocking', classname: 'idVolume_Blocking', displayname: 'Arena Floor',
       decl: 'edit = {\n\tclipModelInfo = {\n\t\tsize = { x = 512; y = 512; z = 16; }\n\t}\n\taffectsNavmesh = true;\n}'},
  11: {inherit: 'snapmaps/volume/blocking', classname: 'idVolume_Blocking', displayname: 'Cover Crate',
       decl: 'edit = {\n\tclipModelInfo = {\n\t\tsize = { x = 64; y = 64; z = 48; }\n\t}\n\tblockDemons = true;\n}'},
  12: {inherit: 'snapmaps/volume/blocking', classname: 'idVolume_Blocking', displayname: 'Sized By Def',
       decl: 'edit = {\n\tisVisible = true;\n}'},
  13: {inherit: 'snapmaps/ai', classname: 'idAI_Snapmap', displayname: 'Heavy Gunner',
       decl: 'edit = {\n\thealth = 300;\n}'}
};
const sent = [];
let dirty = false;

const prefix =
  'var allEntities = [], primaryEid = -1, PREVIEW = true;\n' +
  'function esc(s){ return String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;"); }\n' +
  'function toast(t, k){ toasts.push({text:t, kind:k}); }\n' +
  'function isDirty(){ return dirty(); }\n' +
  'function configSet(){}\n';
const suffix =
  '\nexports.setEntities = function(v){ allEntities = v; };' +
  '\nexports.setPrimary = function(v){ primaryEid = v; };' +
  '\nexports.onList = navOnList;' +
  '\nexports.onShow = navOnShow;' +
  '\nexports.receiveState = navReceiveState;' +
  '\nexports.receiveSetting = navReceiveSetting;' +
  '\nexports.saveResult = navConsumeSaveResult;' +
  '\nexports.toggle = navToggle;' +
  '\nexports.render = navRender;' +
  '\nexports.readFlag = navReadFlag;' +
  '\nexports.readSize = navReadSize;' +
  '\nexports.rows = function(){ return navOrder.map(function(e){ return navRows[e]; }); };' +
  '\nexports.setEnabled = function(v){ navEnabled = v; };';

const api = {};
const toasts = [];
function post(o) {
  sent.push(o);
  if (o.cmd === 'select') {
    const s = STATES[o.eid] || {decl: '', classname: '', inherit: '', displayname: ''};
    api.receiveState({kind: 'state', auto: false, eid: o.eid, ok: true, truncated: false,
                      decl: s.decl, classname: s.classname, inherit: s.inherit, displayname: s.displayname});
  }
}
new Function('exports', 'document', 'post', 'toasts', 'dirty', prefix + nav + dp + suffix)(
  api, document, post, toasts, function () { return dirty; });

/* absent means false, present-and-true means true */
check(api.readFlag('edit = {\n\tisVisible = true;\n}') === false,
  'a volume with no affectsNavmesh line does not read as unmarked');
check(api.readFlag('edit = {\n\taffectsNavmesh = true;\n}') === true,
  'an explicitly marked volume does not read as marked');
check(api.readFlag('edit = {\n\taffectsNavmesh = false;\n}') === false,
  'an explicitly false affectsNavmesh does not read as unmarked');
check(api.readFlag('') === false, 'an empty decl source does not read as unmarked');
check(api.readFlag('edit = {\n\trenderModelInfo = {\n\t\tmodel = "affectsNavmesh";\n\t}\n}') === false,
  'a quoted string is being mistaken for the affectsNavmesh leaf');

check(JSON.stringify(api.readSize(STATES[10].decl)) === JSON.stringify({x: 512, y: 512, z: 16}),
  'clipModelInfo.size is not read back as the box extent');
check(api.readSize(STATES[12].decl) === null,
  'a volume with no clipModelInfo.size should report no size of its own, not a guessed one');

/* empty and idle states */
api.render();
check(nodes.navList.innerHTML.indexOf('Waiting for the map') >= 0,
  'the section does not idle on "waiting for the map" before the first list');
api.setEntities([]);
api.onList({kind: 'list', editorReady: false, entities: []});
check(nodes.navList.innerHTML.indexOf('Open the SnapMap editor') >= 0,
  'there is no "not in the editor" state');
api.onList({kind: 'list', editorReady: true, entities: []});
check(nodes.navList.innerHTML.indexOf('No map loaded') >= 0, 'there is no "no map loaded" state');

api.setEntities([{eid: 13, id: 'ai', name: 'Heavy Gunner', hidden: false}]);
api.onShow();
check(api.rows().length === 0, 'a non-blocking entity was listed as a Blocking Box');
check(nodes.navList.innerHTML.indexOf('no Blocking Boxes') >= 0,
  'there is no "map has no Blocking Boxes" state');

api.setEntities([
  {eid: 11, id: 'b11', name: 'Cover Crate', hidden: false},
  {eid: 12, id: 'b12', name: 'Sized By Def', hidden: false},
  {eid: 13, id: 'ai', name: 'Heavy Gunner', hidden: false}
]);
sent.length = 0;
api.onShow();
check(api.rows().length === 2, 'the scan did not keep exactly the Blocking Boxes');
check(nodes.navSummary.textContent.indexOf('No boxes marked') >= 0,
  'there is no "no boxes marked" state');
check(nodes.navList.innerHTML.indexOf('default size') >= 0,
  'a volume with no size of its own does not say so');
check(nodes.navBadge.textContent === 2, 'the badge does not count the listed volumes');
check(sent.filter(function (o) { return o.cmd === 'select'; })
        .map(function (o) { return o.eid; }).join(',') === '11,12,13',
  'the scan did not read every entity exactly once, in list order');
check(sent.filter(function (o) { return o.cmd === 'save'; }).length === 0,
  'reading the map wrote to it');

/* a republished list with the same entity set must not re-scan (a commit republishes the list) */
sent.length = 0;
api.onList({kind: 'list', editorReady: true, entities: []});
check(sent.length === 0, 'an unchanged entity set triggered a fresh scan of the whole map');

/* a read that did not succeed decides nothing */
api.receiveState({kind: 'state', auto: true, eid: 11, ok: false, truncated: false,
                  decl: '', classname: '', inherit: '', displayname: ''});
api.receiveState({kind: 'state', auto: true, eid: 12, ok: true, truncated: true,
                  decl: '', classname: '', inherit: '', displayname: ''});
check(api.rows().length === 2, 'a failed or truncated read dropped a volume from the list');

/* the filter narrows the rendered rows without losing the logical list */
nodes.navFilter.value = 'cover';
api.render();
check((nodes.navList.innerHTML.match(/class="entity-item nav-item/g) || []).length === 1 &&
      api.rows().length === 2, 'filtering dropped rows from the logical list');
nodes.navFilter.value = 'zzz';
api.render();
check(nodes.navList.innerHTML.indexOf('No matches') >= 0, 'a filter with no hits has no state');
nodes.navFilter.value = '';
api.render();

/* toggling on writes a bare true into the volume's own edit block and commits it */
sent.length = 0;
api.toggle(11, true);
const commit = sent.filter(function (o) { return o.cmd === 'save'; });
check(commit.length === 1, 'toggling a volume did not issue exactly one commit');
check(commit[0].eid === 11 && commit[0].classname === 'idVolume_Blocking' &&
      commit[0].inherit === 'snapmaps/volume/blocking' && commit[0].displayname === 'Cover Crate',
  'the commit does not carry the identity the read round-trip returned');
check(/\n\taffectsNavmesh = true;\n\}/.test(commit[0].decl),
  'the commit did not add a bare true affectsNavmesh leaf to the edit block');
check(commit[0].decl.indexOf('blockDemons = true;') >= 0 &&
      commit[0].decl.indexOf('size = { x = 64; y = 64; z = 48; }') >= 0,
  'the commit rewrote parts of the decl it had no business touching');
check(nodes.navList.innerHTML.indexOf('class="entity-item nav-item busy"') >= 0,
  'a row with a commit in flight is not shown as busy');

api.saveResult({kind: 'saveResult', result: 1});
check(toasts.length === 1 && toasts[0].kind === 'ok', 'a successful mark is not confirmed');
check(nodes.navSummary.textContent.indexOf('1 of 2') >= 0,
  'the summary does not report how many volumes are marked');

/* toggling back off writes a bare false, not a deletion */
sent.length = 0;
api.toggle(11, false);
const off = sent.filter(function (o) { return o.cmd === 'save'; })[0];
check(off && /affectsNavmesh = false;/.test(off.decl),
  'unmarking does not write an explicit false');
api.saveResult({kind: 'saveResult', result: 1});

/* a refused commit puts the row back rather than lying about the map */
sent.length = 0;
toasts.length = 0;
api.toggle(12, true);
api.saveResult({kind: 'saveResult', result: 0});
check(api.rows().filter(function (r) { return r.eid === 12; })[0].marked === false,
  'a refused commit left the row claiming a mark the map never took');
check(toasts.length === 1 && toasts[0].kind === 'err', 'a refused commit is not reported');

/* an unsaved Entity State edit for the same entity blocks the commit */
sent.length = 0;
toasts.length = 0;
api.setPrimary(11);
dirty = true;
api.toggle(11, true);
check(sent.filter(function (o) { return o.cmd === 'save'; }).length === 0,
  'toggling committed over an unsaved Entity State edit');
check(toasts.length === 1 && toasts[0].kind === 'warn', 'the blocked toggle is not explained');
dirty = false;
api.setPrimary(-1);

/* the master switch */
check(api.receiveSetting({kind: 'configValue', key: 'entities.show_hidden', result: 1, valueJson: 'true'}) === false,
  'the Navigation section is claiming another section\'s setting');
api.receiveSetting({kind: 'configValue', key: 'navmesh.enabled', result: 1, valueJson: 'false'});
check(nodes.navEnabled.checked === false, 'a saved navmesh.enabled=false does not reach the control');
check(nodes.navSummary.textContent.indexOf('is off') >= 0,
  'the section does not say the feature is switched off');
check(nodes.navList.innerHTML.indexOf('disabled') >= 0,
  'rows stay editable while the feature is switched off');
api.receiveSetting({kind: 'configValue', key: 'navmesh.enabled', result: 0, valueJson: ''});
check(nodes.navEnabled.checked === true,
  'an unreadable navmesh.enabled does not fall back to the backend default of true');

if (failures) process.exit(1);
console.log('navigation marking contract tests passed');
