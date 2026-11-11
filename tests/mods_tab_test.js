/* The Mods tab (mods_tab.js): the payload model (v4), the two row states, the two kinds of groups
   (Your mods with its organizing folders, one group per downloaded map), the one glyph per row, the
   filter and the attention filter, the details sections (what a mod adds, changes and borrows, its
   console settings and files, the plain cause, the same-files hint), the vocabulary rule
   over every text a player reads, the empty state and New mod saying Refresh picks up edits, the map
   modal and the dry-run-driven confirmations, the Files view, and the page and host contract around
   it. tests/fixtures/mods_tab_payload_v4.json is the
   document the backend writes (its keys are checked by modui_test.c against the backend) and the
   browser preview's sample data. A minimal DOM stands in for the browser. Pure ASCII. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');
const web = path.join(root, 'src', 'ui', 'webview');
const modsTab = require(path.join(web, 'mods_tab.js'));
const v4 = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', 'mods_tab_payload_v4.json'), 'utf8'));
const html = fs.readFileSync(path.join(web, 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const standin = fs.readFileSync(path.join(web, 'preview_standin.js'), 'utf8').replace(/\r\n/g, '\n');
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
  /* The focusable controls a modal's Tab trap asks for (the one selector mods_tab.js uses). */
  querySelectorAll(sel) {
    const tags = sel.split(',').map(t => t.trim()).filter(t => /^[a-z]+$/.test(t));
    const out = [];
    const walk = n => { n.children.forEach(c => { if (tags.indexOf(c.tagName) >= 0 || c.attrs.tabindex === '0') out.push(c); walk(c); }); };
    walk(this);
    return out;
  }
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
             'modsFolderBtn', 'modsNewBtn', 'modsUninstallBtn', 'modsRefreshBtn',
             'modsMapBtn', 'modsMapModal', 'modsMapHost', 'modsUseBtn', 'modsFilesBtn', 'modsFilesModal', 'modsFilesHost',
             'modsNewModal', 'modsNewName', 'modsNewNote', 'modsNewCancel', 'modsNewCreate'];
const GONE = ['modsRebuildBtn', 'modsRemoveBtn', 'modsMap', 'modsMapHead', 'modsDetailHead', 'modsRescanBtn', 'modsVerifyOneBtn', 'modsPaletteBtn',
              'modsPruneBtn', 'modsShadows', 'modsRemoved', 'modsRemovedText', 'modsEmptyBtn', 'modsMigrateBtn', 'modsVerifyBtn'];
const str = v => typeof v === 'string' ? v : (v === null || v === undefined ? '' : String(v));
const num = v => { const n = Number(v); return isFinite(n) ? n : 0; };
const arr = v => Array.isArray(v) ? v : [];
const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const plural = (n, one, many) => n + ' ' + (n === 1 ? one : (many || one + 's'));
const READS = {modFiles: true, modFileText: true};
/* ---- a fake host ---------------------------------------------------------------------------- */

/* The tests' backend: it holds a copy of a document, answers each modsAction as the host does
   (the action's result, then a fresh document) and changes its copy the way the backend would, so
   the page is exercised by the same render-from-the-document path as in the game. handle(message)
   returns the messages the host would post. folders (optional): {key: {files: [modFiles entries],
   texts: {path: text}}}, the files of some mods; any other mod lists mod.json. */
function standIn(initial, folders) {
  var docu = JSON.parse(JSON.stringify(initial || {}));
  var disk = JSON.parse(JSON.stringify(folders || {}));
  function fileEntry(path, size, cls, extra) {
    var f = {path: path, size: size, kind: /\.(decl|json|txt|md|cfg|entities)$/i.test(path) ? 'text' : 'binary', 'class': cls,
             game: false, container: '', inFolder: true, cause: ''};
    Object.keys(extra || {}).forEach(function (k) { f[k] = extra[k]; });
    return f;
  }
  function folderOf(b) {
    if (has(disk, b.key)) return disk[b.key];
    return {files: [fileEntry('mod.json', 400, 'descriptor')], texts: {}};
  }
  /* New mod's example, as the backend writes it (modstore.c ms_write_example). */
  function exampleFolder(key, name) {
    var def = 'assets/generated/decls/entitydef/mods/' + key + '/example_crate.decl';
    var row = 'assets/generated/decls/snapeditorentitydef/mods/' + key + '/example_crate.decl';
    var texts = {};
    texts['mod.json'] = '{\r\n  "name": ' + JSON.stringify(name) + ',\r\n  "description": "",\r\n  "author": "",\r\n' +
      '  "strings": {\r\n    "en": {\r\n      "' + key + '_category": ' + JSON.stringify(name) + ',\r\n' +
      '      "' + key + '_example_crate_name": "Example crate",\r\n' +
      '      "' + key + '_example_crate_desc": "A crate your new mod adds. Change it in the mod\'s files."\r\n    }\r\n  }\r\n}\r\n';
    texts[def] = '{\r\n\tclass = "idProp_Coop";\r\n\tedit = {\r\n\t\trenderModelInfo = {\r\n\t\t\tmodel = "models/snapmaps/props/new/crate01.lwo";\r\n' +
      '\t\t}\r\n\t\tclipModelInfo = {\r\n\t\t\ttype = "CLIPMODEL_AUTO";\r\n\t\t}\r\n\t\tdynamicObstacle = true;\r\n\t}\r\n}\r\n';
    texts[row] = '{\r\n\tinherit = "prop/prop_base";\r\n\tedit = {\r\n' +
      '\t\tpreviewImage = "textures/guis/snapmaps/entity_icons/prop/crate01";\r\n' +
      '\t\tmetaCategoryNameTag = "#str_snapentity_metacategory_toybox";\r\n' +
      '\t\tcategoryNameTag = "#str_' + key + '_category";\r\n' +
      '\t\tdisplayNameTag = "#str_' + key + '_example_crate_name";\r\n' +
      '\t\tdisplayDescriptionTag = "#str_' + key + '_example_crate_desc";\r\n' +
      '\t\tentityDef = "mods/' + key + '/example_crate";\r\n' +
      '\t\trenderInfo = {\r\n\t\t\trenderModelName = "models/snapmaps/props/new/crate01.lwo";\r\n\t\t\talwaysDrawBounds = false;\r\n' +
      '\t\t\tsize = {\r\n\t\t\t\tx = 54;\r\n\t\t\t\ty = 54;\r\n\t\t\t\tz = 54;\r\n\t\t\t}\r\n\t\t}\r\n' +
      '\t\tflags = {\r\n\t\t\tplaceable = true;\r\n\t\t}\r\n\t}\r\n}\r\n';
    return {row: row, folder: {files: [fileEntry('mod.json', texts['mod.json'].length, 'descriptor'),
                                        fileEntry(def, texts[def].length, 'add'), fileEntry(row, texts[row].length, 'add')], texts: texts}};
  }
  function clone() { return JSON.parse(JSON.stringify(docu)); }
  function mods() { return Array.isArray(docu.mods) ? docu.mods : (docu.mods = []); }
  function modOf(key) { return mods().filter(function (b) { return b.key === key; })[0] || null; }
  function mapMods() { return docu.map && Array.isArray(docu.map.mods) ? docu.map.mods : []; }
  function mapModOf(key) { return mapMods().filter(function (r) { return (r.key || r.salt) === key; })[0] || null; }
  function nameOf(key, name) { var b = modOf(key); return b ? (b.name || b.folder || b.key) : (name || key); }
  function recount() {
    mods().forEach(function (b) { b.state = b.cause ? 'attention' : 'ready'; });
    if (docu.map && Array.isArray(docu.map.mods)) {
      docu.map.vanilla = docu.map.mods.length === 0;
    }
  }
  function mapPart(r) {
    var left = mapMods().length - 1;
    return {name: str(docu.map && docu.map.name), entities: arr(r.placedEntities).slice(), entityCount: num(r.placedCount),
            references: arr(r.references).slice(), manifestRow: true, attached: !!r.attached,
            tagRemoved: left === 0,
            vanillaAfter: left === 0, published: !!(docu.map.published && docu.map.published.state === 'published')};
  }
  /* Uninstall moves the folder to the Recycle Bin; the saved maps are not the preview's to count. */
  function installPart(b, open) {
    return {folder: b.key, disposition: 'recycle', savedMaps: num(b.savedMaps), undoCleared: !!open};
  }
  function removeFromMap(r) {
    docu.map.mods = mapMods().filter(function (x) { return x !== r; });
    docu.map.entities = Math.max(0, num(docu.map.entities) - num(r.placedCount));
  }
  return function handle(o) {
    var a = str(o && o.action), key = str(o && o.arg), ok = true, msg = '', data = {action: a};
    if (a === 'openFolder') return [];
    if (has(READS, a)) {
      /* The Files view's reads: the result alone, as the host answers them. */
      var tab = key.indexOf('\t'), mk = tab < 0 ? key : key.slice(0, tab), fp = tab < 0 ? '' : key.slice(tab + 1);
      var mb = modOf(mk), fo = mb ? folderOf(mb) : null;
      data.key = mk;
      if (!mb) { ok = false; msg = 'That mod is no longer installed.'; }
      else if (a === 'modFiles') {
        data.name = str(mb.name);
        data.cause = str(mb.cause);
        data.files = JSON.parse(JSON.stringify(fo.files));
        data.truncated = false;
      } else {
        var ent = fo.files.filter(function (f) { return f.path === fp; })[0];
        if (!ent) { ok = false; msg = "The mod's folder does not hold this file; it is read from your install."; }
        else if (ent.kind !== 'text') { ok = false; msg = 'This file is binary; it has no text to show.'; }
        else if (!ent.inFolder) { ok = false; msg = "The mod's folder does not hold this file; it is read from your install."; }
        else {
          data.path = fp;
          data.text = has(fo.texts, fp) ? fo.texts[fp] : '// Browser preview: in the game this is the file\'s text from the mods folder.\r\n';
          data.size = data.text.length;
          data.productBlock = fp === 'mod.json';
          data.cause = str(ent.cause);
        }
      }
      data.ok = ok;
      data.message = msg;
      return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}];
    }
    if (a === 'newMod') {
      var nmName = key.trim();
      if (!nmName) { ok = false; msg = 'No mod was created: a new mod needs a name.'; }
      else {
        var nk = nmName.replace(/[\\\/:*?"<>|]/g, '').trim() || 'mod', base = nk, ex, salt = 'k3x9', n = 2;
        for (; modOf(nk); n++) nk = base + ' (' + n + ')';
        salt = ('newmod' + (mods().length % 100)).slice(0, 8);
        ex = exampleFolder(salt, nmName);
        disk[nk] = ex.folder;
        mods().push({key: nk, salt: salt, name: nmName, author: '', size: 1530, files: 3,
                     state: 'ready', cause: '', editorRows: 1, folder: nk, group: '', place: '', placeTitle: '',
                     description: '', requirements: {cvars: {}}, editorRowNames: ['Example crate'], publishedMaps: 0,
                     facts: {adds: 2, borrows: 0, surfaces: 0, editor: 0, replaces: 0, editorOnly: false, editorTweaks: [],
                             replacements: [], edits: [], registries: {}},
                     borrows: {paths: [], count: 0}, warnings: []});
        data.key = nk;
        data.file = ex.row;
        msg = 'Created ' + nmName + ' in mods\\' + nk + '. Its Example crate is in the Toybox. After you change its files, press Refresh to see the change.';
      }
      recount();
      data.ok = ok;
      data.message = msg;
      return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}, {kind: 'mods', data: clone()}];
    }
    var r = mapModOf(key), b = modOf(key);
    if (a === 'rescan') msg = '';
    else if (a === 'addToMine') {
      if (!b) { ok = false; msg = 'That mod is no longer installed.'; }
      else if (!b.place) msg = nameOf(key) + ' is already one of your mods.';
      else {
        var mine = JSON.parse(JSON.stringify(b));
        mine.key = mine.folder = b.folder;
        mine.place = mine.placeTitle = '';
        mods().push(mine);
        data.key = mine.key;
        msg = 'Added ' + nameOf(key) + ' to your mods, in mods\\' + mine.key + '; your own maps can use it now.';
      }
    } else if (a === 'mapAddMod') {
      if (!docu.map || docu.map.entities === -1 || !Array.isArray(docu.map.mods)) { ok = false; msg = 'No map is open in the editor.'; }
      else if (!b) { ok = false; msg = key + ' is not installed.'; }
      else if (b.place) { ok = false; msg = nameOf(key) + ' came with the map ' + b.placeTitle + '. Add it to your mods first (Add to my mods); then your own maps can use it.'; }
      else if (r && r.attached) msg = nameOf(key) + ' is already used by the whole of ' + docu.map.name + '.';
      else {
        if (r) r.attached = true;
        else docu.map.mods.push({key: key, salt: str(b.salt), name: str(b.name), state: 'installed',
                                 placedEntities: [], placedCount: 0, placedModules: 0, references: [], size: num(b.size),
                                 inManifest: true, viaNeeds: false, attached: true, nothingPlaced: true, build: ''});
        msg = nameOf(key) + ' is now used by the whole of ' + docu.map.name + '; save the map to keep it.';
      }
    } else if (a === 'chooseRegistry') {
      var tab = key.indexOf('\t'), slot = tab < 0 ? key : key.slice(0, tab), pick = tab < 0 ? '' : key.slice(tab + 1);
      var cl = docu.map && Array.isArray(docu.map.registryClashes) ? docu.map.registryClashes.filter(function (c) { return c.slot === slot; })[0] : null;
      if (!cl) { ok = false; msg = 'The open map has no choice for ' + slot + '.'; }
      else {
        var chosenName = (cl.providers.filter(function (p) { return p.id === pick; })[0] || {}).name || 'the chosen mod';
        cl.chosen = pick;
        cl.serving = pick || (cl.providers[0] && cl.providers[0].id) || '';
        msg = pick ? 'This map now uses ' + chosenName + '\'s version of ' + slot + '; it applies now and is saved with the map\'s next save.'
                   : slot + ": the map's choice is cleared; the first mod in the map's list serves it.";
      }
    } else if (a === 'mapRemoveModPreview') {
      if (!r) { ok = false; msg = 'The open map does not use ' + key + '.'; }
      else data.preview = {action: 'mapRemoveMod', key: key, name: nameOf(r.key, r.name), map: mapPart(r), install: null};
    } else if (a === 'uninstallPreview') {
      if (!b) { ok = false; msg = key + ' is not installed.'; }
      else data.preview = {action: 'uninstall', key: key, name: nameOf(key), map: r ? mapPart(r) : null,
                           install: installPart(b, !!(docu.map && docu.map.entities !== -1)),
                           editorRows: arr(b.editorRowNames).slice()};
    } else if (a === 'mapRemoveMod') {
      if (!r) { ok = false; msg = 'The open map does not use ' + key + '.'; }
      else { msg = 'Removed ' + nameOf(r.key, r.name) + ' from ' + docu.map.name + ': ' + plural(num(r.placedCount), 'object') + ' deleted.'; removeFromMap(r); }
    } else if (a === 'uninstall') {
      if (!b) { ok = false; msg = key + ' is not installed.'; }
      else {
        var open = !!(docu.map && docu.map.entities !== -1);
        msg = 'Uninstalled ' + nameOf(key) + (r ? ' and removed it from the open map' : '') + '. Its folder was moved to the Recycle Bin' +
              (open ? ' and the undo history was cleared.' : '.');
        if (r) removeFromMap(r);
        docu.mods = mods().filter(function (x) { return x !== b; });
      }
    } else { ok = false; msg = 'Browser preview: the mods store is not available here.'; }
    recount();
    data.ok = ok;
    data.message = msg;
    return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}, {kind: 'mods', data: clone()}];
  };
}


/* store: the fake host above, answering every modsAction synchronously; without one the test
   answers by hand. */
function harness(opts) {
  opts = opts || {};
  const nodes = {};
  IDS.forEach(id => { nodes[id] = new FakeNode(id === 'modsAttnBtn' || /Btn$/.test(id) ? 'button' : 'div'); nodes[id].id = id; });
  nodes.modsMapModal.style.display = 'none';
  nodes.modsMapModal.appendChild(nodes.modsMapHost);
  nodes.modsFilesModal.style.display = 'none';
  nodes.modsFilesModal.appendChild(nodes.modsFilesHost);
  nodes.modsNewModal.style.display = 'none';
  const sent = [], toasts = [], menus = [];
  const saved = [];   /* what a page that still wrote mods.collapsed_groups would have saved */
  /* The document: the page's body, the element with focus, and its own keydown listeners. */
  const doc = new FakeNode('#document');
  doc.body = new FakeNode('body');
  doc.createElement = tag => new FakeNode(tag);
  doc.createElementNS = (ns, tag) => Object.assign(new FakeNode(tag), {ns});
  Object.defineProperty(doc, 'activeElement', {get: () => activeElement});
  let ui = null;
  ui = modsTab.create({
    document: doc, byId: id => nodes[id] || null, saveCollapsed: list => saved.push(list),
    post: m => { sent.push(m); if (opts.store) opts.store(m).forEach(r => ui.onMessage(r)); },
    toast: (t, k) => toasts.push({text: t, kind: k}),
    tokens: opts.tokens, menu: (x, y, items) => menus.push({x, y, items}), copy: opts.copy
  });
  ui.bind();
  return {ui, nodes, sent, toasts, saved, menus, doc};
}
/* A key pressed where focus is: the focused node's listeners, then the document's (the bubble). */
function press(h, key, extra) {
  const at = activeElement || h.doc.body;
  const e = at.dispatch('keydown', Object.assign({key}, extra || {}));
  let n = at.parentNode;
  for (; n; n = n.parentNode) (n.listeners.keydown || []).forEach(f => f.call(n, e));
  if (at !== h.doc) (h.doc.listeners.keydown || []).forEach(f => f.call(h.doc, e));
  return e;
}
/* A node removed from the page loses focus to the body, as a browser's does. */
function attached(h, n) { for (; n; n = n.parentNode) if (n === h.nodes.modsFilesModal || n === h.doc.body) return true; return false; }
function rows(h) { return h.nodes.modsList.all('mods-row'); }
function rowOf(h, key) { return rows(h).find(n => n.getAttribute('data-key') === key); }
function keys(h) { return rows(h).map(n => n.getAttribute('data-key')); }
function iconOf(n) { return n.all('mods-status-icon')[0].getAttribute('data-status'); }
function section(h, name, box) { return (box || h.nodes.modsDetail).all('mods-section').find(n => n.getAttribute('data-section') === name); }
function modalText(h) { return h.nodes.modsMapHost.textContent; }
function modalButtons(h) { return h.nodes.modsMapHost.tags('button').filter(b => !/mods-map-close/.test(b.className)); }
/* As the host answers a rescan: its result, then the document. */
function answer(h, data) {
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', data: {action: 'rescan', ok: true, message: ''}});
  h.ui.onMessage({kind: 'mods', data: data});
}
function shown(h, data) { h.ui.show(); if (!h.ui.state().loaded) answer(h, data || v4); }
function live(data) { const h = harness({store: standIn(data || v4)}); h.ui.show(); return h; }

/* ---- the model -------------------------------------------------------------------------------- */
test('normalize: the v4 fixture', () => {
  const m = modsTab.normalize(v4);
  assert.equal(m.ok, true);
  assert.equal(m.v, 4);
  assert.equal(m.mods.length, 8);
  assert.deepEqual(m.mods.map(b => b.name), ['Arena Gates', 'Arena Gates (old)', 'Broken Barrels', 'Doors', 'Editor Grid',
                                             'Hell Guards', 'Lava Floor', 'R2 Imp copy'], 'by name without case');
  const imp = m.mods.find(b => b.key === 'alex.r2-imp');
  assert.equal(imp.salt, 'alexrimp');
  assert.deepEqual(imp.borrows, {count: 2, paths: ['models/monsters/imp/imp.lwo', 'generated/decls/material/models/monsters/imp/imp.decl']});
  assert.equal(imp.facts.edits.length, 2);
  const hg = m.mods.find(b => b.name === 'Hell Guards');
  assert.equal(hg.place, 'maps\\Hell Guard Arena');
  assert.equal(hg.placeTitle, 'Hell Guard Arena');
  const gates = m.mods.find(b => b.key === 'Arena Gates');
  assert.deepEqual(gates.sameFilesAs, {name: 'Arena Gates (old)', folder: 'Arena Gates (old)', count: 5});
  assert.deepEqual(gates.cvars, [{name: 'g_useImageBlackList', value: '0'}, {name: 'g_useResourceBlackList', value: '0'}]);
  assert.deepEqual(gates.commands, {activate: ['echo gates on'], deactivate: ['echo gates off']});
  assert.equal(m.mods.find(b => b.key === 'Broken Barrels').facts, null, 'a mod that is not built carries no facts');
  assert.equal(m.map.open, true);
  assert.equal(m.map.mods.length, 3);
  assert.equal(m.map.mods[2].name, 'Spiked Walls');
});
test('normalize: hostile and missing values', () => {
  assert.equal(modsTab.normalize(null).ok, false);
  assert.equal(modsTab.normalize([]).ok, false);
  const m = modsTab.normalize({mods: [{key: 7, state: 'weird', sameFilesAs: {name: 'x', count: 0}, borrows: 'no'}, null]});
  assert.equal(m.mods.length, 2);
  assert.ok(m.mods.every(b => b.state === 'attention'), 'an unknown state needs attention');
  assert.equal(m.mods[0].sameFilesAs, null);
  assert.deepEqual(m.mods[0].borrows, {count: 0, paths: []});
  assert.equal(modsTab.normalize({storeReadable: false}).error, 'The mods folder cannot be read.');
});
test('two states: ready or needs attention, the cause one sentence from the backend', () => {
  const m = modsTab.normalize(v4);
  const broken = m.mods.find(b => b.key === 'Broken Barrels');
  const info = modsTab.statusInfo(broken);
  assert.equal(info.tone, 'warn');
  assert.equal(info.sentence, broken.cause, 'the backend\'s sentence, verbatim');
  assert.equal(modsTab.statusInfo(m.mods.find(b => b.key === 'alex.r2-imp')).sentence, modsTab.READY_TEXT);
  assert.equal(modsTab.statusInfo(m.mods.find(b => b.name === 'Hell Guards')).sentence,
               'Came with the map Hell Guard Arena; only that map uses it.');
  assert.equal(modsTab.statusInfo({state: 'attention', cause: ''}).sentence, 'This mod cannot be used right now; press Refresh.');
});
test('formatting: sizes, the row line', () => {
  assert.equal(modsTab.formatSize(0), '0 B');
  assert.equal(modsTab.formatSize(2048), '2.0 KB');
  assert.equal(modsTab.formatSize(1843230), '1.8 MB');
  assert.equal(modsTab.subText({size: 1843230}), '1.8 MB');
  assert.equal(modsTab.subText({version: '1.0.0', size: 2048}), '2.0 KB');   /* a mod has no version */
  assert.equal(modsTab.bareName('mods/alexrimp/single_imp', 'alexrimp'), 'single_imp');
  assert.equal(modsTab.bareName('ai/imp', 'alexrimp'), 'ai/imp');
});
test('action messages', () => {
  assert.deepEqual(modsTab.actionMessage('rescan'), {cmd: 'modsAction', action: 'rescan'});
  assert.deepEqual(modsTab.actionMessage('mapAddMod', 'alex.r2-imp'), {cmd: 'modsAction', action: 'mapAddMod', arg: 'alex.r2-imp'});
  for (const gone of ['migrate', 'verify', 'remove', 'pruneList', 'pruneApply', 'palette', 'rebuild'])
    assert.equal(modsTab.actionMessage(gone), null, gone + ' is not a page action');
});

/* ---- the vocabulary rule ---------------------------------------------------------------------- */
const BANNED = /\b(salt|salts|hash|hashes|bundle|bundles|key|keys|pointer|pointers|surface|surfaces|legacy|migrat\w*)\b|\bbuild \d|\b[0-9a-f]{16,}\b/i;
function readable(h) {
  /* every text a player reads in the tab: the list, the details, the status line, the modal, the toasts */
  return [h.nodes.modsList.textContent, h.nodes.modsDetail.textContent, h.nodes.modsStatus.textContent,
          h.nodes.modsMapHost.textContent].concat(h.toasts.map(t => t.text)).join('\n');
}
test('the vocabulary: no salt, hash, bundle, key, pointer, surface, legacy, migrate or build number anywhere a player reads', () => {
  const h = live();
  const salts = v4.mods.map(b => b.salt);
  const check = where => {
    const t = readable(h);
    assert.ok(!BANNED.test(t), where + ': ' + (t.match(BANNED) || [''])[0]);
    salts.forEach(s => assert.ok(t.indexOf(s) < 0, where + ': the salt ' + s));
  };
  check('the list');
  for (const b of modsTab.normalize(v4).mods) {
    h.ui.select(b.key);
    /* open every expandable line too: their names are read */
    check('details of ' + b.name);
  }
  h.nodes.modsMapBtn.click();
  check('the map modal');
  h.nodes.modsMapHost.all('mods-map-row')[0].tags('button').find(x => x.textContent === 'Remove from map').click();
  check('the confirmation');
  h.ui.closeMap();
  h.nodes.modsRefreshBtn.click();
  check('after Refresh');
  /* the page's own texts */
  const own = [modsTab.READY_TEXT, modsTab.REFRESH_TEXT, modsTab.EMPTY_TEXT, modsTab.BORROWS_NOTE, modsTab.NEEDS_NOTE,
               modsTab.EDITOR_ONLY_NOTE, modsTab.NEW_MOD_HINT, modsTab.folderRuleText('Crate Pack'), modsTab.NOTHING_CHANGED_TEXT,
               modsTab.EDITOR_TWEAKS_TEXT, modsTab.MAP_LEVEL_TEXT, modsTab.NOTHING_PLACED_TEXT]
    .concat(Object.values(modsTab.FILE_CLASSES), Object.values(modsTab.FILE_SOURCES), Object.values(modsTab.SCOPE),
            Object.values(modsTab.UNINSTALL_HEAD));
  own.forEach(t => assert.ok(!BANNED.test(t), t));
});

/* ---- the list --------------------------------------------------------------------------------- */
test('show rescans once; the list waits for the answer', () => {
  const h = harness();
  assert.equal(h.sent.length, 0, 'nothing is requested until the tab is shown');
  h.ui.show();
  h.ui.show();
  assert.deepEqual(h.sent, [{cmd: 'modsAction', action: 'rescan'}], 'a second show while waiting does not ask again');
  assert.match(h.nodes.modsList.textContent, /Reading your mods/);
  answer(h, v4);
  assert.equal(h.toasts.length, 0, 'a successful rescan on show says nothing');
  h.ui.show();
  assert.equal(h.sent.length, 2, 'showing the tab again rereads the folder');
  assert.equal(h.ui.onMessage({kind: 'other'}), false);
});
test('two kinds of groups: Your mods with its folders, and one group per downloaded map', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsBadge.textContent, '8');
  const areas = h.nodes.modsList.children;
  assert.equal(areas.length, 2);
  assert.equal(areas[0].getAttribute('data-group'), 'area:own');
  assert.equal(areas[0].all('pf-fname')[0].textContent, 'Your mods');
  assert.equal(areas[0].all('pf-fcount')[0].textContent, '6');
  assert.equal(areas[1].getAttribute('data-group'), 'map:maps\\Hell Guard Arena');
  assert.equal(areas[1].all('pf-fname')[0].textContent, 'Hell Guard Arena', 'titled with the map\'s name');
  assert.equal(areas[1].all('pf-fcount')[0].textContent, '2');
  /* inside Your mods: the organizing folder first, then the mods at the top */
  const folder = areas[0].all('mods-folder')[0];
  assert.equal(folder.getAttribute('data-group'), 'campaign-interactables');
  assert.equal(folder.all('mods-row')[0].getAttribute('data-key'), 'campaign-interactables\\doors');
  assert.deepEqual(keys(h), ['campaign-interactables\\doors', 'Arena Gates', 'Arena Gates (old)', 'Broken Barrels', 'Editor Grid',
                             'alex.r2-imp', 'maps\\Hell Guard Arena\\Hell Guards', 'maps\\Hell Guard Arena\\Lava Floor']);
  assert.deepEqual(h.ui.state().view.map(b => b.key), keys(h), 'the arrow keys walk the shown order');
  /* each row: the name and size, one glyph */
  const imp = rowOf(h, 'alex.r2-imp');
  assert.equal(imp.all('mods-row-name')[0].textContent, 'R2 Imp copy');
  assert.equal(imp.all('mods-row-sub')[0].textContent, '1.8 MB');
  assert.equal(imp.children.length, 2, 'glyph and text, nothing else');
  assert.ok(rows(h).every(n => n.all('mods-status-icon').length === 1));
  assert.equal(iconOf(imp), 'map', 'the open map uses it');
  assert.equal(iconOf(rowOf(h, 'campaign-interactables\\doors')), 'map');
  assert.equal(iconOf(rowOf(h, 'Broken Barrels')), 'warn');
  assert.equal(rowOf(h, 'Broken Barrels').title, v4.mods.find(b => b.key === 'Broken Barrels').cause);
  assert.equal(iconOf(rowOf(h, 'Editor Grid')), 'ok');
  assert.equal(rowOf(h, 'Editor Grid').title, modsTab.READY_TEXT);
  /* a group closes for the session; a closed group with a mod needing attention shows the triangle */
  areas[0].all('pf-folder-head')[0].click();
  assert.deepEqual(keys(h), ['maps\\Hell Guard Arena\\Hell Guards', 'maps\\Hell Guard Arena\\Lava Floor']);
  assert.equal(h.nodes.modsList.children[0].all('mods-status-icon')[0].getAttribute('data-status'), 'warn');
  /* selecting a mod inside opens it again */
  h.ui.select('alex.r2-imp');
  assert.ok(rowOf(h, 'alex.r2-imp'));
  /* no map place: one group only */
  const own = clone(v4);
  own.mods = own.mods.filter(b => !b.place);
  h.ui.onMessage({kind: 'mods', data: own});
  assert.equal(h.nodes.modsList.children.length, 1);
});
test('the filter matches name, folder and the map\'s name; the attention filter', () => {
  const h = harness();
  shown(h);
  const f = h.nodes.modsFilter;
  f.value = 'arena';
  f.dispatch('input');
  assert.deepEqual(keys(h), ['Arena Gates', 'Arena Gates (old)', 'maps\\Hell Guard Arena\\Hell Guards', 'maps\\Hell Guard Arena\\Lava Floor']);
  f.value = 'zzz';
  f.dispatch('input');
  assert.match(h.nodes.modsList.textContent, /No mod matches the filter\./);
  f.value = '';
  f.dispatch('input');
  const at = h.nodes.modsAttnBtn;
  assert.equal(at.style.display, '');
  assert.equal(at.title, '1 mod needs attention: show only these');
  at.click();
  assert.deepEqual(keys(h), ['Broken Barrels']);
  assert.equal(at.getAttribute('aria-pressed'), 'true');
  at.click();
  assert.equal(keys(h).length, 8);
});
test('keyboard selection walks the shown rows', () => {
  const h = harness();
  shown(h);
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(h.ui.state().selectedKey, 'campaign-interactables\\doors');
  h.nodes.modsList.dispatch('keydown', {key: 'End'});
  assert.equal(h.ui.state().selectedKey, 'maps\\Hell Guard Arena\\Lava Floor');
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowUp'});
  assert.equal(h.ui.state().selectedKey, 'maps\\Hell Guard Arena\\Hell Guards');
});
test('empty: say how to start and that Refresh picks up edits; New mod in the card', () => {
  const h = harness({store: standIn(Object.assign(clone(v4), {mods: []}))});
  h.ui.show();
  const card = h.nodes.modsList.all('mods-empty')[0];
  assert.ok(card.textContent.indexOf(modsTab.EMPTY_TEXT) >= 0);
  assert.ok(card.textContent.indexOf(modsTab.REFRESH_TEXT) >= 0);
  assert.equal(modsTab.REFRESH_TEXT, "After you change a mod's files, press Refresh to see the change.");
  const b = card.all('mods-new-btn')[0];
  b.click();
  assert.equal(h.nodes.modsNewModal.style.display, 'flex');
  h.nodes.modsNewCancel.click();
  assert.equal(activeElement, b, 'focus returns to the button');
  /* the details pane's How mods work says it too */
  assert.ok(h.nodes.modsDetail.textContent.indexOf(modsTab.REFRESH_TEXT) >= 0);
});

/* ---- the details pane ------------------------------------------------------------------------- */
test('details: description, what it adds, what it changes, what it borrows, console settings, files', () => {
  const h = live();
  h.ui.select('alex.r2-imp');
  const d = h.nodes.modsDetail;
  assert.equal(d.all('mods-detail-name')[0].textContent, 'R2 Imp copy');
  assert.equal(d.all('mods-detail-sentence')[0].textContent, modsTab.READY_TEXT);
  assert.deepEqual(d.all('mods-section').map(n => n.getAttribute('data-section')),
                   ['about', 'adds', 'changes', 'borrows', 'requirements', 'files', 'sharing']);
  assert.match(section(h, 'about').textContent, /^About Description A copy of the Imp/);
  assert.match(section(h, 'about').textContent, /Folder alex\.r2-imp$/);
  assert.equal(section(h, 'adds').textContent, 'What it adds 2 Toybox rows Imp (R2) Imp (R2, caged)');
  const ch = section(h, 'changes');
  assert.match(ch.textContent, /^What it changes in the game Edits 2 game files; a map that uses this mod gets the game's text with these edits\./);
  assert.match(ch.textContent, /entitydef\/player\/ coop : +3 fields set/);
  assert.match(ch.textContent, /Adds 1 encounter to the encounter list of a map that uses it\. encounter +single_imp/);
  /* a strings key the game defines changes that text (DECISIONS 9) */
  assert.match(ch.textContent, /Changes the text of 1 game string in a map that uses it\./);
  const bw = section(h, 'borrows').textContent;
  assert.match(bw, /^What it borrows from the game 2 game files, read from your install models\/monsters\/imp\/ +imp\.lwo /);
  assert.ok(bw.endsWith(modsTab.BORROWS_NOTE));
  assert.equal(section(h, 'borrows').tags('details').length, 1, 'the count opens to the list');
  assert.match(section(h, 'requirements').textContent, /^Console settings Needs no console settings\. /);
  assert.match(section(h, 'files').textContent, /^Files In its folder 13 files, 1\.8 MB Show the files$/);
  assert.equal(section(h, 'sharing').textContent, 'Sharing Published maps Used by 1 of your published maps');
  /* console settings and commands */
  h.ui.select('Arena Gates');
  assert.match(section(h, 'requirements').textContent, /Needs g_useImageBlackList +0 g_useResourceBlackList +0/);
  assert.match(section(h, 'requirements').textContent, /Commands when used echo gates on Commands when no longer used echo gates off/);
  assert.match(section(h, 'changes').textContent, /Loads the game's sfx_vega sounds, which SnapMap leaves out, for a map that uses it\. sfx_vega : loaded \(41 ms\)/);
  assert.equal(section(h, 'warnings').textContent, 'Warnings ' + v4.mods[1].warnings[0]);
  /* nothing changed, nothing borrowed */
  h.ui.select('Arena Gates (old)');
  assert.equal(section(h, 'changes').textContent, 'What it changes in the game ' + modsTab.NOTHING_CHANGED_TEXT);
  assert.equal(section(h, 'borrows').textContent, 'What it borrows from the game Borrows nothing; every file is its own.');
});
test('details: the same-files hint is informational', () => {
  const h = live();
  h.ui.select('Arena Gates');
  const hint = h.nodes.modsDetail.all('mods-same')[0];
  assert.equal(hint.textContent, 'Carries 5 files that Arena Gates (old) also carries. They stay two separate mods.');
  assert.equal(iconOf(rowOf(h, 'Arena Gates')), 'ok', 'a hint, not attention');
  h.ui.select('alex.r2-imp');
  assert.equal(h.nodes.modsDetail.all('mods-same').length, 0);
});
test('details: a mod that cannot be used says what is wrong and what to do, plainly', () => {
  const h = live();
  h.ui.select('Broken Barrels');
  const s = h.nodes.modsDetail.all('mods-detail-sentence')[0];
  assert.match(s.className, /warn/);
  assert.match(s.textContent, /^This mod could not be built: .*; fix that in its folder and press Refresh\.$/);
  assert.deepEqual(h.nodes.modsDetail.all('mods-section').map(n => n.getAttribute('data-section')),
                   ['about', 'adds', 'requirements', 'files', 'sharing'], 'no derived facts for a mod that is not built');
  assert.equal(h.nodes.modsUseBtn.disabled, true, 'a mod that cannot be used is not added to a map');
});
test('details: a downloaded map\'s mod has the short view: its name, its map, Add to my mods and Remove', () => {
  const h = live();
  h.ui.select('maps\\Hell Guard Arena\\Hell Guards');
  const d = h.nodes.modsDetail;
  assert.equal(d.all('mods-detail-name')[0].textContent, 'Hell Guards');
  assert.equal(d.all('mods-detail-sentence')[0].textContent, 'Came with the map Hell Guard Arena; only that map uses it.');
  assert.deepEqual(d.all('mods-section').map(n => n.getAttribute('data-section')), ['about'], 'nothing written for authors');
  assert.equal(section(h, 'about').textContent, 'About Came with Hell Guard Arena ' + modsTab.PLACE_CLEANUP_TEXT);
  assert.equal(h.nodes.modsFilesBtn.style.display, 'none');
  assert.equal(h.nodes.modsUninstallBtn.textContent, 'Remove');
  assert.equal(h.nodes.modsUninstallBtn.disabled, false);
  const add = h.nodes.modsUseBtn;
  assert.equal(add.textContent, 'Add to my mods');
  assert.equal(add.style.display, '');
  assert.equal(add.disabled, false);
  assert.equal(add.title, modsTab.ADD_MINE_TITLE);
  add.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'addToMine', arg: 'maps\\Hell Guard Arena\\Hell Guards'});
  assert.match(h.toasts.pop().text, /^Added Hell Guards to your mods, in mods\\Hell Guards; your own maps can use it now\.$/);
  assert.equal(h.ui.state().selectedKey, 'Hell Guards', 'your new copy is selected');
  assert.equal(h.nodes.modsUseBtn.textContent, 'Use in this map');
  assert.equal(h.nodes.modsUninstallBtn.textContent, 'Uninstall');
  /* Remove: the uninstall confirmation, named Remove */
  h.ui.select('maps\\Hell Guard Arena\\Lava Floor');
  h.nodes.modsUninstallBtn.click();
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Remove Lava Floor?');
  assert.deepEqual(modalButtons(h).map(b => b.textContent), ['Cancel', 'Remove']);
});
test('an author never needs the salt: no Copy mod id, and the folder rule said where they look', () => {
  const h = harness();
  shown(h);
  h.ui.select('alex.r2-imp');
  assert.equal(h.nodes.modsDetail.all('mods-copy-id').length, 0);
  assert.ok(!/Copy mod id|mod id/i.test(source), 'no Copy mod id and no "mod id" anywhere in the page code');
  assert.equal(modsTab.folderRuleText('Crate Pack'),
               'Put new files under assets\\mods\\Crate Pack\\, and in your files write mods/Crate Pack/ and plain string names.');
  h.ui.select('');
  assert.ok(h.nodes.modsDetail.textContent.indexOf(modsTab.folderRuleText('')) >= 0, 'How mods work says it too');
});
test('details: a folder of editor tweaks only has no map action', () => {
  const h = live();
  h.ui.select('Editor Grid');
  assert.equal(h.nodes.modsUseBtn.style.display, 'none');
  assert.ok(section(h, 'requirements').textContent.includes(modsTab.EDITOR_ONLY_NOTE));
  assert.match(section(h, 'changes').textContent, /Tweaks 1 editor file on this computer only/);
  h.ui.select('alex.r2-imp');
  assert.equal(h.nodes.modsUseBtn.style.display, '');
  assert.ok(section(h, 'requirements').textContent.includes(modsTab.NEEDS_NOTE));
});
test('details colour: own paths, game files, edit counts, settings and Toybox rows', () => {
  assert.deepEqual(modsTab.pathParts('generated/decls/entitydef/mods/alexrimp/imp_copy.decl', ['alexrimp']).map(p => [p.text, p.cls]),
                   [['generated/decls/entitydef/', 'mk-dir'], ['mods/alexrimp/', 'mk-own'], ['imp_copy.decl', '']]);
  assert.deepEqual(modsTab.pathParts('models/monsters/imp/imp.lwo', ['alexrimp']).map(p => [p.text, p.cls]),
                   [['models/monsters/imp/', 'mk-dir'], ['imp.lwo', 'mk-game']]);
  assert.equal(modsTab.valueClass('0'), 'mk-num');
  assert.equal(modsTab.valueClass('on'), 'mk-str');
  const h = live();
  h.ui.select('alex.r2-imp');
  assert.ok(section(h, 'adds').all('mk-str').length === 2, 'Toybox rows in the string hue');
  assert.ok(section(h, 'changes').all('mk-set').length >= 1, 'what an edit sets');
});

/* ---- buttons and actions ---------------------------------------------------------------------- */
test('the header and the details head: the buttons the tab has, and no others', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsNewBtn.style.display || '', '', 'New mod always shows');
  assert.equal(h.nodes.modsUninstallBtn.disabled, true, 'nothing selected');
  h.ui.select('alex.r2-imp');
  assert.equal(h.nodes.modsUninstallBtn.disabled, false);
  h.nodes.modsRefreshBtn.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'rescan'});
  assert.equal(h.ui.state().busy, 'rescan');
  assert.ok(h.nodes.modsInstallBtn.disabled && h.nodes.modsRefreshBtn.disabled, 'held while busy');
  assert.equal(h.nodes.modsStatus.textContent, 'Reading your mods and building what changed...', 'a build shows its progress');
  h.ui.onMessage({kind: 'modsProgress', action: 'rescan', phase: 'game'});
  assert.equal(h.nodes.modsStatus.textContent, 'Bringing the change into the game...');
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', ok: true, message: '', data: {}});
  assert.equal(h.ui.state().busy, '');
  assert.equal(h.nodes.modsStatus.textContent, '', 'a Refresh with nothing left to do says nothing');
  /* Install asks folders or ZIPs, then the host's picker opens */
  h.nodes.modsInstallBtn.click();
  const menu = h.menus.pop();
  assert.deepEqual(menu.items.map(i => i.label), ['Install mod folders...', 'Install mod ZIPs...']);
  menu.items[0].run();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'install'});
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', ok: false, data: {cancelled: true}});
  menu.items[1].run();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'install', arg: 'zip'});
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', ok: false, data: {cancelled: true}});
  h.nodes.modsFolderBtn.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'openFolder'});
  h.ui.select('campaign-interactables\\doors');
  h.nodes.modsDetail.all('mods-link')[0].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'openFolder', arg: 'campaign-interactables\\doors'});
});
test('Refresh shows what a change still needs, even when quiet; a game part that did not run says so', () => {
  const h = harness();
  shown(h);
  h.nodes.modsRefreshBtn.click();
  const sentence = 'Some of the changes take effect only after you restart DOOM.';
  h.ui.onMessage({kind: 'modsActionResult', action: 'rescan', ok: true, message: sentence,
                  data: {refresh: {needsRestart: 1, needsMapReload: 0}}});
  assert.equal(h.nodes.modsStatus.textContent, sentence);
  assert.deepEqual(h.toasts.pop(), {text: sentence, kind: 'ok'});
  h.ui.onMessage({kind: 'mods', data: v4});
  assert.equal(h.nodes.modsStatus.textContent, sentence, 'the document that follows keeps the sentence');
  h.nodes.modsNewBtn.click();
  h.nodes.modsNewName.value = 'Late';
  h.nodes.modsNewCreate.click();
  h.ui.onMessage({kind: 'modsActionResult', action: 'newMod', applyPending: true, ok: true, message: 'Created Late in mods\\Late.',
                  data: {key: 'Late'}});
  assert.equal(h.toasts.pop().text, 'Created Late in mods\\Late. ' + modsTab.APPLY_PENDING_TEXT);
});
test('the map modal: the open map\'s details and mods, in plain words', () => {
  const h = live();
  const ov = h.nodes.modsMapModal;
  h.nodes.modsMapBtn.click();
  assert.equal(ov.style.display, 'flex');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Hell Guard Arena');
  const t = modalText(h);
  for (const s of ['Name Hell Guard Arena', 'Published Yes, 2026-', 'Its mods Uploaded with the published map',
                   'Mods 3 mods, 5.4 MB for a player to download'])
    assert.ok(t.indexOf(s) >= 0, s);
  assert.ok(t.indexOf('Saved map') < 0, 'no saved-map id');
  const mr = h.nodes.modsMapHost.all('mods-map-row');
  assert.equal(mr.length, 3);
  assert.match(mr[0].textContent, /^R2 Imp copy 1\.8 MB 3 placed entities use it Also used by timeline: Wave 2: spawn the imps, wire: Relay: all imps dead Remove from map$/);
  assert.match(mr[1].textContent, /^Doors 3\.6 MB Used by the whole map, nothing placed/);
  assert.equal(iconOf(mr[2]), 'warn');
  assert.match(mr[2].textContent, /^Spiked Walls 51 KB 2 placed entities use it; not installed on this computer Remove from map$/);
  /* a choice between mods names the mods, never their ids */
  const opts = h.nodes.modsMapHost.tags('option').map(o => o.textContent);
  assert.deepEqual(opts, ['The first mod the map lists (R2 Imp copy)', 'R2 Imp copy', 'Doors']);
  assert.ok(modalText(h).indexOf('Search tag') < 0, 'no search tag row: the product keeps it in step');
  h.nodes.modsMapHost.tags('select')[0].value = 'doorsx8q';
  h.nodes.modsMapHost.tags('select')[0].dispatch('change');
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'chooseRegistry', arg: 'entitydef/player/coop/edit/handsModelMPUseDoomHands\tdoorsx8q'});
  assert.match(h.toasts.pop().text, /^This map now uses Doors's version of /);
  /* Escape closes; focus returns to the opener */
  const e = ov.dispatch('keydown', {key: 'Escape'});
  assert.equal(e.defaultPrevented, true);
  assert.equal(ov.style.display, 'none');
  assert.equal(activeElement, h.nodes.modsMapBtn);
  /* no map open */
  const closed = clone(v4);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  const c = live(closed);
  c.nodes.modsMapBtn.click();
  assert.match(modalText(c), /This map Open a map in the editor to see the mods it uses\./);
  assert.equal(c.nodes.modsUseBtn.style.display, 'none');
});
test('Remove from map: the dry run drives the confirmation; the document drives the result', () => {
  const h = live();
  h.nodes.modsMapBtn.click();
  const removeBtn = () => h.nodes.modsMapHost.all('mods-map-row')[0].tags('button').find(b => b.textContent === 'Remove from map');
  removeBtn().click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveModPreview', arg: 'alex.r2-imp'});
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Remove R2 Imp copy from Hell Guard Arena?');
  const items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.match(items[0], /^Deletes 3 placed entities from Hell Guard Arena\. Imp \(R2\) 1/);
  assert.match(items[1], /^Severs 2 wire or timeline references to those entities\./);
  assert.deepEqual(modalButtons(h).map(b => b.textContent), ['Cancel', 'Remove from map']);
  modalButtons(h)[1].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveMod', arg: 'alex.r2-imp'});
  assert.equal(h.nodes.modsMapHost.all('mods-map-row').length, 2);
  assert.match(h.toasts.pop().text, /^Removed R2 Imp copy from Hell Guard Arena: 3 objects deleted\./);
  assert.equal(iconOf(rowOf(h, 'alex.r2-imp')), 'ok', 'the row loses the in-this-map glyph');
});
test('a choice between mods can be cleared; Remove from map is the one wording', () => {
  const h = live();
  h.nodes.modsMapBtn.click();
  const sel = () => h.nodes.modsMapHost.tags('select')[0];
  assert.equal(sel().value, '', 'no choice yet: the first mod the map lists decides');
  sel().value = 'doorsx8q';
  sel().dispatch('change');
  assert.equal(sel().value, 'doorsx8q');
  sel().value = '';
  sel().dispatch('change');
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'chooseRegistry', arg: 'entitydef/player/coop/edit/handsModelMPUseDoomHands\t'});
  assert.equal(sel().value, '', 'cleared');
  assert.ok(!/Delete from map/.test(source), 'every control says Remove from map');
});
test('Use in this map from the details pane, and back', () => {
  const h = live();
  h.ui.select('Arena Gates');
  const use = h.nodes.modsUseBtn;
  assert.equal(use.textContent, 'Use in this map');
  use.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapAddMod', arg: 'Arena Gates'});
  assert.match(h.toasts.pop().text, /^Arena Gates is now used by the whole of Hell Guard Arena; save the map to keep it\.$/);
  assert.equal(use.textContent, 'Remove from map');
  use.click();
  assert.equal(h.ui.state().modal.page, 'confirm');
  modalButtons(h)[1].click();
  assert.equal(h.ui.state().modal.open, false, 'started from the details pane: the dialog closes');
  assert.equal(use.textContent, 'Use in this map');
  /* a downloaded map's mod: refused by the fake host as by the backend */
  h.ui.act('mapAddMod', 'maps\\Hell Guard Arena\\Lava Floor');
  assert.match(h.toasts.pop().text, /came with the map Hell Guard Arena\. Add it to your mods first \(Add to my mods\)/);
});
test('Uninstall: the confirmation lists what it does; the folder goes to the Recycle Bin', () => {
  const h = live();
  h.ui.select('alex.r2-imp');
  h.nodes.modsUninstallBtn.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'uninstallPreview', arg: 'alex.r2-imp'});
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Uninstall R2 Imp copy?');
  const items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.ok(items.indexOf('Removes 2 Toybox rows from the editor (Imp (R2), Imp (R2, caged)).') >= 0);
  assert.equal(items[items.length - 1], 'Moves the folder mods\\alex.r2-imp to the Windows Recycle Bin.');
  assert.equal(h.nodes.modsMapHost.all('mods-scope')[0].textContent, modsTab.SCOPE.uninstallUsed);
  modalButtons(h)[1].click();
  assert.equal(rowOf(h, 'alex.r2-imp'), undefined);
  assert.equal(h.ui.state().selectedKey, '');
  assert.equal(h.nodes.modsBadge.textContent, '7');
  /* the consequence lines of the dry run's fields */
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall',
    install: {folder: 'maps\\Hell Guard Arena\\Hell Guards', savedMaps: 0, keptBuild: 'Hell Guards'}})).map(c => c.text),
    ['Moves the folder mods\\maps\\Hell Guard Arena\\Hell Guards to the Windows Recycle Bin.',
     'Another folder of this mod stays installed (mods\\Hell Guards); maps that use the mod open with it.']);
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall', install: {folder: 'a', savedMaps: 3}})).map(c => c.text),
    ['Moves the folder mods\\a to the Windows Recycle Bin.',
     '3 of your saved maps use it; they will not open until you restore it from the Recycle Bin.']);
});

/* ---- New mod and the Files view --------------------------------------------------------------- */
test('New mod: a name, the folder made with its example, Refresh named as the way to pick up edits', () => {
  const h = harness({store: standIn(v4)});
  h.ui.show();
  h.nodes.modsNewBtn.click();
  assert.equal(h.nodes.modsNewModal.style.display, 'flex');
  assert.equal(h.nodes.modsNewNote.textContent, "Snapmap+ makes a folder for it in mods\\ with an example crate you can place right away.");
  h.nodes.modsNewName.value = '   ';
  h.nodes.modsNewCreate.click();
  assert.ok(!h.sent.some(m => m.action === 'newMod'));
  assert.equal(h.toasts.pop().text, 'Give the new mod a name.');
  h.nodes.modsNewName.value = 'Crate Pack';
  h.nodes.modsNewModal.dispatch('keydown', {key: 'Enter'});
  assert.deepEqual(h.sent.filter(m => m.action === 'newMod'), [{cmd: 'modsAction', action: 'newMod', arg: 'Crate Pack'}]);
  const st = h.ui.state();
  assert.equal(st.selectedKey, 'Crate Pack', 'the folder is named after the mod');
  assert.ok(rowOf(h, 'Crate Pack'));
  assert.match(h.toasts.slice(-1)[0].text, /After you change its files, press Refresh to see the change\.$/);
  assert.equal(st.files.open, true);
  assert.match(st.files.selected, /^assets\/generated\/decls\/snapeditorentitydef\/mods\/[a-z0-9]+\/example_crate\.decl$/);
  const pane = h.nodes.modsFilesHost.all('mf-file')[0];
  assert.equal(pane.all('mf-hint')[0].textContent, modsTab.NEW_MOD_HINT + ' ' + modsTab.folderRuleText('Crate Pack'));
  assert.match(modsTab.NEW_MOD_HINT, /press Refresh in the Mods tab/);
  /* the list's right-click entry */
  const e = h.nodes.modsList.dispatch('contextmenu', {pageX: 40, pageY: 90});
  assert.equal(e.defaultPrevented, true);
  assert.deepEqual(h.menus.map(m => m.items.map(i => i.label)), [['New mod...']]);
});
const IMP = 'assets/generated/decls/entitydef/mods/alexrimp/imp_copy.decl';
const FOLDERS = {'alex.r2-imp': {files: [
  {path: 'mod.json', size: 900, kind: 'text', class: 'descriptor', game: false, container: '', inFolder: true, cause: ''},
  {path: IMP, size: 1200, kind: 'text', class: 'add', game: false, container: '', inFolder: true, cause: ''},
  {path: 'assets/generated/decls/entitydef/mods/alexrimp/broken.decl', size: 0, kind: 'text', class: '', game: false, container: '',
   inFolder: true, cause: 'This mod could not be built: the file is empty; fix that in its folder and press Refresh.'},
  {path: 'assets/models/monsters/imp/imp.lwo', size: 2048, kind: 'binary', class: 'borrow', game: true,
   container: 'gameresources', inFolder: false, cause: ''},
  {path: 'assets/generated/decls/entitydef/player/coop.decl', size: 300, kind: 'text', class: 'edit', game: false,
   container: '', inFolder: true, cause: ''}],
  texts: {'mod.json': '{\r\n  "name": "R2 Imp copy"\r\n}\r\n', [IMP]: '{\r\n\tinherit = "ai/demon/imp";\r\n}\r\n'}}};
test('the Files modal: the folder tree with the salt in its paths, one glyph per file, the text read-only', () => {
  const h = harness({store: standIn(v4, FOLDERS)});
  h.ui.show();
  h.ui.select('alex.r2-imp');
  h.nodes.modsFilesBtn.click();
  assert.equal(h.nodes.modsFilesModal.style.display, 'flex');
  assert.deepEqual(h.sent.filter(m => m.action === 'modFiles'), [{cmd: 'modsAction', action: 'modFiles', arg: 'alex.r2-imp'}]);
  const st = h.ui.state();
  assert.equal(st.busy, '', 'a read never holds the page busy');
  assert.equal(st.files.selected, 'assets/generated/decls/entitydef/mods/alexrimp/broken.decl', 'the file a problem names first');
  const host = h.nodes.modsFilesHost;
  assert.equal(host.all('modal-title')[0].textContent, 'R2 Imp copy');
  assert.equal(host.all('mf-state')[0].textContent, modsTab.READY_TEXT);
  const names = host.all('mf-list')[0].all('pf-fname').map(n => n.textContent);
  assert.ok(names.indexOf('mods/alexrimp') >= 0, 'the Files view shows the salt in its paths');
  const pane = host.all('mf-file')[0];
  assert.equal(pane.all('prob')[0].textContent, 'This mod could not be built: the file is empty; fix that in its folder and press Refresh.');
  assert.equal(pane.all('mf-meta')[0].children[0].textContent, 'Not part of the mod; Snapmap+ ignores it');
  h.ui.selectFile(IMP);
  const ta = host.all('mf-file')[0].all('code-editor')[0].tags('textarea')[0];
  assert.equal(ta.readOnly, true);
  assert.equal(ta.value, '{\n\tinherit = "ai/demon/imp";\n}\n');
  h.ui.selectFile('assets/models/monsters/imp/imp.lwo');
  assert.deepEqual(host.all('mf-meta')[0].children.map(n => n.textContent),
                   ["A game file under this mod's own name", "from the game's campaign and multiplayer files", '2.0 KB']);
  h.nodes.modsFilesModal.dispatch('keydown', {key: 'Escape'});
  assert.equal(h.nodes.modsFilesModal.style.display, 'none');
  assert.equal(activeElement, h.nodes.modsFilesBtn);
});
test('the Files view: Open in editor hands a file of the folder to the system', () => {
  const h = harness({store: standIn(v4, FOLDERS)});
  h.ui.show();
  h.ui.select('alex.r2-imp');
  h.nodes.modsFilesBtn.click();
  h.ui.selectFile(IMP);
  const open = h.nodes.modsFilesHost.all('mf-open')[0];
  assert.equal(open.textContent, 'Open in editor');
  open.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'openFile', arg: 'alex.r2-imp\t' + IMP});
  assert.equal(h.ui.state().busy, '', 'opening a file never holds the page');
  h.ui.selectFile('assets/models/monsters/imp/imp.lwo');
  assert.equal(h.nodes.modsFilesHost.all('mf-open').length, 0, 'a game file the folder does not hold has nothing to open');
});
test('the Files view model: classes, sources and the tree', () => {
  const d = modsTab.normFiles({key: 'k', files: [{path: 'b\\x.decl', kind: 'text', class: 'mystery', container: 'mars', game: 'yes'},
                                                  {path: ''}, {path: 'a.json', kind: 'text', class: 'descriptor'}]});
  assert.deepEqual(d.files.map(f => [f.path, f.cls, f.container, f.game, f.text]), [['a.json', 'descriptor', '', false, true], ['b/x.decl', '', '', false, true]]);
  const t = modsTab.fileTree([{path: 'assets/generated/decls/entitydef/mods/k/a.decl', cause: 'x'},
                              {path: 'assets/generated/decls/material/mods/k/c.decl', cause: ''}], '');
  assert.equal(t.dirs[0].name, 'assets/generated/decls');
  assert.deepEqual(t.dirs[0].dirs.map(x => [x.name, x.count, x.attn]), [['entitydef', 1, 1], ['material', 1, 0]]);
});
test('the fake host answers as the host does', () => {
  const store = standIn(v4);
  const r = store({cmd: 'modsAction', action: 'rescan'});
  assert.deepEqual(r.map(m => m.kind), ['modsActionResult', 'mods']);
  assert.notEqual(r[1].data, v4, 'a copy, never the original');
  assert.deepEqual(store({cmd: 'modsAction', action: 'openFolder'}), []);
  assert.equal(store({cmd: 'modsAction', action: 'install'})[0].ok, false);
  assert.deepEqual(store({cmd: 'modsAction', action: 'uninstallPreview', arg: 'maps\\Hell Guard Arena\\Hell Guards'})[0].data.preview.editorRows,
    ['Hell Guard (hammer)', 'Hell Guard (shield)', 'Hell Guard (fist)']);
  assert.equal(v4.mods.length, 8, 'the fixture is untouched');
});

/* ---- the page and host contract --------------------------------------------------------------- */
test('the header fits at the minimum pane width', () => {
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<div class="statusbar">'));
  const head = panel.slice(panel.indexOf('<div class="panel-head">'), panel.indexOf('<div class="panel-toolbar">'));
  const rule = sel => { const at = css.indexOf('\n  ' + sel + ' {'); assert.ok(at >= 0, sel); return css.slice(at, css.indexOf('}', at)); };
  const px = (text, re) => Number(text.match(re)[1]);
  const button = px(rule('.btn.icon'), /width: (\d+)px/);
  const gap = px(rule('.panel-actions'), /gap: (\d+)px/);
  const headGap = px(rule('.panel-head'), /gap: (\d+)px/);
  const inset = px(css, /--pane-inline: (\d+)px/);
  const min = Number(panel.match(/data-min-before="(\d+)"[^>]*data-split-label="Mods"/)[1]);
  const buttons = (head.match(/<button /g) || []).length;
  assert.equal(buttons, 6, 'attention, This map, Refresh, Open folder, New mod, Install');
  assert.ok(2 * inset + headGap + buttons * button + (buttons - 1) * gap <= min, 'six buttons fit in a ' + min + ' px pane');
});
test('page contract', () => {
  assert.ok(html.includes('<button type="button" class="tab" role="tab" id="tab-mods" data-tab="mods" aria-controls="panel-mods" aria-selected="false" tabindex="-1">Mods</button>'));
  IDS.forEach(id => assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id));
  GONE.forEach(id => assert.ok(!html.includes('id="' + id + '"'), 'mockup.html no longer has #' + id));
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<div class="statusbar">'));
  const head = panel.slice(panel.indexOf('<div class="panel-head">'), panel.indexOf('<div class="panel-toolbar">'));
  const actions = head.slice(head.indexOf('<div class="panel-actions">'));
  for (const id of ['modsAttnBtn', 'modsMapBtn', 'modsRefreshBtn', 'modsFolderBtn', 'modsNewBtn', 'modsInstallBtn'])
    assert.match(actions, new RegExp('<button type="button" class="btn (primary )?icon icon-button( mods-attn)?" id="' + id + '"'), id + ' is a panel icon button');
  for (const [id, ic] of [['modsFolderBtn', 'folder-open'], ['modsNewBtn', 'folder-plus'], ['modsInstallBtn', 'download'], ['modsMapBtn', 'map'], ['modsRefreshBtn', 'refresh-cw']])
    assert.match(panel, new RegExp('id="' + id + '"[^>]*>(<svg class="ui-icon"[^>]*><use href="#icon-' + ic + '">)'), id + ' is the ' + ic + ' icon');
  assert.match(panel, /id="modsRefreshBtn" title="Refresh: after you change a mod's files, build them again and bring the change into the game"/);
  const detailHead = panel.slice(panel.indexOf('<span class="panel-title">Mod Details</span>'), panel.indexOf('id="modsDetail"'));
  assert.deepEqual((detailHead.match(/<button [^>]*id="(\w+)"/g) || []).map(t => t.match(/id="(\w+)"/)[1]),
                   ['modsFilesBtn', 'modsUseBtn', 'modsUninstallBtn']);
  assert.ok(!/migrat|verify/i.test(panel), 'no Migrate and no Verify in the tab');
  assert.match(html, /id="modsList" role="listbox" tabindex="0"/);
  assert.equal(html.split('<script src="mods_tab.js"></script>').length, 2, 'one module tag');
  assert.match(html, /if \(which === 'mods'\) modsUi\.show\(\);/);
  /* the browser preview's data is the v4 fixture; the release page carries no stand-in */
  const m = standin.match(/\nvar PREVIEW_MODS = (\{.*\});\r?\n/);
  assert.ok(m, 'PREVIEW_MODS is one JSON line');
  assert.deepEqual(JSON.parse(m[1]), v4, 'PREVIEW_MODS is the v4 fixture');
  assert.ok(!/standIn|stand-in/.test(source), 'mods_tab.js, which the release page carries, holds no stand-in host');
  const pf = standin.match(/\nvar PREVIEW_MOD_FILES = (\{.*\});\r?\n/);
  assert.ok(pf, 'PREVIEW_MOD_FILES is one JSON line');
  assert.ok(JSON.parse(pf[1])['alex.r2-imp'].files.every(f => f.path && f.kind && 'class' in f), 'the preview lists files as modFiles does');
  assert.ok(!/setInterval|setTimeout|requestAnimationFrame/.test(source), 'the tab never polls or defers');
  assert.ok(!/innerHTML/.test(source), 'all text goes through textContent');
  assert.ok(!/[^\x00-\x7f]/.test(source), 'ASCII only');
  assert.ok(!/migrat/i.test(source), 'the Migrate code is gone');
  /* the payload spec */
  for (const k of ['"salt"', '"place"', '"placeTitle"', '"borrows"', '"sameFilesAs"', '"cause"', '"editorRowNames"', '"facts"',
                   '"requirements"', '"publishedMaps"', '"warnings"', 'mapRemoveModPreview', 'uninstallPreview', 'mapAddMod',
                   '"placedEntities"', '"references"', '"vanilla"', '"savedMaps"', '"undoCleared"', '"attached"', 'addToMine', 'openFile'])
    assert.ok(docs.indexOf(k) >= 0, 'docs/webview-ui.md documents ' + k);
});
test('host and interface contract', () => {
  assert.ok(host.includes('} else if (cmd == L"modsAction") {'));
  assert.ok(host.includes('SH_IFACE_HAS_SLOT(g_iface, mods)'), 'an older backend is never called past its table');
  for (const a of modsTab.ACTIONS.filter((x) => x !== 'openFolder' && x !== 'openFile'))
    assert.ok(host.includes('if (name == L"' + a + '")'), 'every page action reaches the backend: ' + a);
  assert.ok(host.includes('if (act == L"openFolder") ui_mods_open_folder(arg);'), 'openFolder opens the named mod folder');
  assert.ok(host.includes('else if (act == L"openFile") ui_mods_open_file(arg);'), 'openFile opens one file of a mod folder');
  assert.ok(host.includes('ui_mods_relative_file(rel)'), 'openFile takes a path below the mod folder');
  assert.ok(host.includes('ui_mods_relative_folder(folder)'), 'openFolder takes a folder under the mods folder by its relative path');
  assert.ok(host.includes('ui_mods_post_result(L"unknown", false, L"Unknown mods action.", nullptr);'));
});

console.log('mods_tab_test: OK (' + passed + ' tests)');
