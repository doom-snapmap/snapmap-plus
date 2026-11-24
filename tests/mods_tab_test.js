/* The Mods tab (mods_tab.js): the payload model (v4), the two row states, the list's groups (Your
   mods with its organizing folders, then Maps on this computer from the content answer), the one
   glyph per row, the filter and the attention filter, the details sections (the In this map block,
   what a mod adds, changes and borrows, its console settings and files, the plain cause, the
   same-files hint), each state of a mod and the open map and its one action, the vocabulary rule
   over every text a player reads, the empty state and New mod saying Refresh picks up edits, the map
   modal and the dry-run-driven confirmations, the Files view, the Settings menu's scripts setting,
   and the page and host contract around
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
/* The host reads a page request's route, action and argument through mods_message.h, which holds its action table. */
const hostMessage = fs.readFileSync(path.join(web, 'mods_message.h'), 'utf8').replace(/\r\n/g, '\n');
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
             'modsNewBtn', 'modsUninstallBtn', 'modsRefreshBtn',
             'modsMapBtn', 'modsMapModal', 'modsMapHost', 'modsFilesBtn', 'modsFilesModal', 'modsFilesHost',
             'modsNewModal', 'modsNewName', 'modsNewNote', 'modsNewCancel', 'modsNewCreate',
             'menuScriptedMaps', 'menuScriptedMapsLabel', 'menuScriptedMapsNote'];
/* The ids of the old markup: the tab's own controls that moved (the folder button, the Use button, the scripts
   checkbox and the content box) and every control retired before them. #modsInMap is built by the tab. */
const GONE = ['modsRebuildBtn', 'modsRemoveBtn', 'modsMap', 'modsMapHead', 'modsDetailHead', 'modsRescanBtn', 'modsVerifyOneBtn', 'modsPaletteBtn',
              'modsPruneBtn', 'modsShadows', 'modsRemoved', 'modsRemovedText', 'modsEmptyBtn', 'modsMigrateBtn', 'modsVerifyBtn',
              'modsFolderBtn', 'modsUseBtn', 'modsScriptedMaps', 'modsScriptedMapsLabel', 'modsScriptedMapsNote', 'modsContent', 'modsInMap'];
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
   texts: {path: text}}}, the files of some mods; any other mod lists mod.json. kept (optional): the
   content answer, {storage, lines, rows}: one row per kept map's mod, its token in rows parallel to
   lines ("" for the storage line); without it the host answers content with no lines. */
function standIn(initial, folders, kept) {
  var docu = JSON.parse(JSON.stringify(initial || {}));
  var disk = JSON.parse(JSON.stringify(folders || {}));
  var held = JSON.parse(JSON.stringify(kept || {storage: '', lines: [], rows: []}));
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
    if (a === 'content') {
      /* The kept maps' content: the result alone, as the host answers it (no fresh document): the storage line and
         one line per map's mod, its token in the parallel rows. */
      if (key === 'removePlayed') {
        held = {storage: 'Map content: 205.8 MB for 1 map.', lines: [], rows: []};
        msg = 'Removed 1.1 GB of content of maps you only played; playing one of them again gets it back.';
      } else msg = held.storage;
      data.ok = true; data.message = msg; data.storage = held.storage;
      data.lines = held.lines.slice();
      data.rows = held.rows.slice();
      return [{kind: 'modsActionResult', action: a, ok: true, message: msg, data: data}];
    }
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
                     state: 'ready', cause: '', editorRows: 1, modules: 0, folder: nk, group: '',
                     description: '', requirements: {cvars: {}}, editorRowNames: ['Example crate'], publishedMaps: 0,
                     facts: {adds: 2, borrows: 0, surfaces: 0, editor: 0, replaces: 0, editorOnly: false, editorTweaks: [],
                             replacements: [], edits: [], registries: {}},
                     borrows: {paths: [], count: 0}, warnings: []});
        data.key = nk;
        data.file = ex.row;
        msg = 'Created ' + nmName + ' in your mods folder, as ' + nk + '. Its Example crate is in the Toybox. After you change its files, press Refresh to see the change.';
      }
      recount();
      data.ok = ok;
      data.message = msg;
      return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}, {kind: 'mods', data: clone()}];
    }
    var r = mapModOf(key), b = modOf(key);
    if (a === 'rescan') msg = '';
    else if (a === 'addToMine') {
      /* The token of a kept map's mod: the folder name it is copied to is the token's last part. */
      var at = held.rows.indexOf(key), made = key.slice(key.lastIndexOf('\\') + 1);
      if (at < 0 || !key) { ok = false; msg = 'That map is no longer on this computer.'; }
      else if (modOf(made)) msg = made + ' is already one of your mods.';
      else {
        mods().push({key: made, salt: 'hellgd3k', name: made, author: '', size: 900, files: 4, state: 'ready', cause: '',
                     editorRows: 1, modules: 0, folder: made, group: '', description: '', requirements: {cvars: {}},
                     editorRowNames: [made], publishedMaps: 0, facts: null, borrows: {paths: [], count: 0}, warnings: []});
        data.key = made;
        msg = 'Added ' + made + ' to your mods, in your folder ' + made + '; your own maps can use it now.';
      }
    } else if (a === 'mapAddMod') {
      if (!docu.map || docu.map.entities === -1 || !Array.isArray(docu.map.mods)) { ok = false; msg = 'No map is open in the editor.'; }
      else if (!b) { ok = false; msg = key + ' is not installed.'; }
      else if (r && r.attached) msg = nameOf(key) + ' is already used by the whole of ' + docu.map.name + '.';
      else {
        if (r) r.attached = true;
        else docu.map.mods.push({key: key, salt: str(b.salt), name: str(b.name), state: 'installed',
                                 placedEntities: [], placedCount: 0, placedModules: 0, references: [], size: num(b.size),
                                 inManifest: true, viaNeeds: false, attached: true, nothingPlaced: true, pinned: false, objects: [], build: ''});
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
  IDS.forEach(id => { nodes[id] = new FakeNode(id === 'modsAttnBtn' || id === 'menuScriptedMaps' || /Btn$/.test(id) ? 'button' : 'div'); nodes[id].id = id; });
  /* modsRoot is a role=button div in the page; the Settings menu's item is a menuitemcheckbox. */
  nodes.modsRoot.setAttribute('role', 'button');
  nodes.menuScriptedMaps.setAttribute('role', 'menuitemcheckbox');
  nodes.menuScriptedMaps.setAttribute('aria-checked', 'true');
  /* An id the tab builds itself (#modsInMap) is found by walking the details pane, as the browser's getElementById finds it. */
  const byId = id => {
    if (nodes[id]) return nodes[id];
    let found = null;
    const walk = n => { n.children.forEach(c => { if (!found && c.id === id) found = c; if (!found) walk(c); }); };
    walk(nodes.modsDetail);
    return found;
  };
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
    document: doc, byId: id => byId(id), saveCollapsed: list => saved.push(list),
    post: m => { sent.push(m); if (opts.store && m.cmd === 'modsAction') opts.store(m).forEach(r => ui.onMessage(r)); },
    toast: (t, k) => toasts.push({text: t, kind: k}),
    /* the tab uses no menu of its own any more; one that is passed in and called shows in menus */
    tokens: opts.tokens, menu: (x, y, items) => menus.push({x, y, items}), copy: opts.copy,
    every: opts.every, cancel: opts.cancel, saveSetting: opts.saveSetting, changed: opts.changed,
    after: opts.after
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
function live(data, kept) { const h = harness({store: standIn(data || v4, null, kept)}); h.ui.show(); return h; }
/* One entry of the open map's mods as the backend writes it: by default a mod the map needs for other objects only
   (nothing placed, not the whole mod). */
function mapRow(o) {
  return Object.assign({key: '', salt: '', name: '', state: 'installed', placedEntities: [], placedCount: 0, placedModules: 0,
                        references: [], size: 1000, inManifest: true, viaNeeds: true, attached: false, nothingPlaced: false,
                        pinned: false, objects: [], build: '', update: ''}, o);
}
/* The fixture with the open map's mods replaced. */
function withMapMods(list) { const d = clone(v4); d.map.mods = list; return d; }
/* The In this map block of the selected mod: its state, texts and buttons. */
function inMap(h) {
  const box = h.nodes.modsDetail.all('mods-inmap')[0];
  return {box, state: box.getAttribute('data-state'), head: box.all('mods-inmap-head')[0].textContent,
          buttons: (box.all('mods-inmap-actions')[0] ? box.all('mods-inmap-actions')[0].tags('button') : [])};
}

/* ---- the model -------------------------------------------------------------------------------- */
test('normalize: the v4 fixture', () => {
  const m = modsTab.normalize(v4);
  assert.equal(m.ok, true);
  assert.equal(m.v, 4);
  assert.equal(m.mods.length, 7);
  assert.deepEqual(m.mods.map(b => b.name), ['Arena Gates', 'Arena Gates (old)', 'Bare Hands', 'Broken Barrels', 'Doors', 'Editor Grid',
                                             'R2 Imp copy'], 'by name without case');
  const imp = m.mods.find(b => b.key === 'alex.r2-imp');
  assert.equal(imp.salt, 'alexrimp');
  assert.deepEqual(imp.borrows, {count: 2, paths: ['models/monsters/imp/imp.lwo', 'generated/decls/material/models/monsters/imp/imp.decl']});
  assert.equal(imp.facts.edits.length, 2);
  /* places are retired: a mod row names no place, and every row counts its modules */
  assert.ok(m.mods.every(b => !('place' in b) && !('placeTitle' in b)), 'the model has no place');
  assert.ok(v4.mods.every(b => !('place' in b) && !('placeTitle' in b) && b.modules === 0), 'the document sends none, and modules 0');
  assert.ok(m.mods.every(b => b.modules === 0));
  assert.ok(!m.mods.some(b => /hell guard|lava floor/i.test(b.name)), 'a downloaded map\'s mods are not in the list');
  const bare = m.mods.find(b => b.key === 'bare-hands');
  assert.deepEqual([bare.salt, bare.editorRows, bare.modules], ['barehand', 0, 0]);
  const gates = m.mods.find(b => b.key === 'Arena Gates');
  assert.deepEqual(gates.sameFilesAs, {name: 'Arena Gates (old)', folder: 'Arena Gates (old)', count: 5});
  assert.deepEqual(gates.cvars, [{name: 'g_useImageBlackList', value: '0'}, {name: 'g_useResourceBlackList', value: '0'}]);
  assert.deepEqual(gates.commands, {activate: ['echo gates on'], deactivate: ['echo gates off']});
  assert.equal(m.mods.find(b => b.key === 'Broken Barrels').facts, null, 'a mod that is not built carries no facts');
  assert.equal(m.map.open, true);
  assert.equal(m.map.mods.length, 4);
  assert.equal(m.map.mods[2].name, 'Spiked Walls');
  /* every map entry says whether the published copy is the map's and which objects it places */
  assert.ok(v4.map.mods.every(r => typeof r.pinned === 'boolean' && Array.isArray(r.objects)));
  assert.deepEqual(m.map.mods[1].objects, ['Blast door', 'Lift door']);
  assert.equal(m.map.mods[1].attached, false);
  assert.deepEqual([m.map.mods[3].key, m.map.mods[3].salt, m.map.mods[3].attached, m.map.mods[3].nothingPlaced, m.map.mods[3].objects],
                   ['bare-hands', 'barehand', true, true, []], 'the open map uses Bare Hands as a whole');
  assert.equal(m.map.mods[0].pinned, false);
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
  assert.equal(modsTab.statusInfo(m.mods.find(b => b.name === 'Bare Hands')).sentence, modsTab.READY_TEXT);
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
               modsTab.EDITOR_TWEAKS_TEXT, modsTab.SCRIPTED_MAPS_LABEL, modsTab.SCRIPTED_MAPS_NOTE]
    .concat(Object.values(modsTab.MAP_STATE), Object.values(modsTab.MAP_LINES),
            Object.values(modsTab.FILE_CLASSES), Object.values(modsTab.FILE_SOURCES), Object.values(modsTab.SCOPE),
            Object.values(modsTab.UNINSTALL_HEAD));
  own.forEach(t => assert.ok(!BANNED.test(t), t));
  assert.equal(modsTab.MAP_LEVEL_TEXT, undefined, 'the map-level and nothing-placed texts are gone');
  assert.equal(modsTab.NOTHING_PLACED_TEXT, undefined);
});
test('the vocabulary holds in every state of a mod and the open map, in the list, the details and the Settings menu', () => {
  const states = {}, texts = [];
  /* the fixture's map plus a mod only other objects need: out, objects, whole, needed and editorOnly all show */
  const data = clone(v4);
  data.map.mods.push(mapRow({key: 'Arena Gates', salt: 'gatesk2m', name: 'Arena Gates', size: 412000}));
  const closed = clone(v4);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  for (const d of [data, closed]) {
    const h = live(d);
    for (const b of modsTab.normalize(d).mods) {
      h.ui.select(b.key);
      states[h.nodes.modsDetail.all('mods-inmap')[0].getAttribute('data-state')] = true;
      texts.push(h.nodes.modsDetail.textContent);
    }
    texts.push(h.nodes.modsList.textContent, h.nodes.menuScriptedMapsLabel.textContent, h.nodes.menuScriptedMapsNote.textContent);
  }
  assert.deepEqual(Object.keys(states).sort(), ['editorOnly', 'needed', 'noMap', 'objects', 'out', 'whole']);
  texts.forEach(t => assert.ok(!BANNED.test(t), (t.match(BANNED) || [''])[0]));
  v4.mods.forEach(b => texts.forEach(t => assert.ok(t.indexOf(b.salt) < 0, 'the salt ' + b.salt)));
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
  assert.equal(h.sent.filter(m => m.action === 'rescan').length, 2, 'showing the tab again rereads the folder');
  assert.equal(h.sent.filter(m => m.action === 'content').length, 1, 'and the map content is read once the first list arrived');
  assert.equal(h.ui.onMessage({kind: 'other'}), false);
});
test('the list: Your mods with its folders, and no group per downloaded map', () => {
  const h = harness();
  shown(h);
  assert.equal(h.nodes.modsBadge.textContent, '7');
  const areas = h.nodes.modsList.children;
  assert.equal(areas.length, 1, 'Your mods alone while the content of maps is not known');
  assert.equal(areas[0].getAttribute('data-group'), 'area:own');
  assert.equal(areas[0].all('pf-fname')[0].textContent, 'Your mods');
  assert.equal(areas[0].all('pf-fcount')[0].textContent, '7');
  assert.ok(!h.nodes.modsList.textContent.includes('Hell Guard Arena'), 'a downloaded map has no group of its mods');
  assert.ok(areas.every(a => !/^map:/.test(a.getAttribute('data-group'))));
  /* inside Your mods: the organizing folder first, then the mods at the top */
  const folder = areas[0].all('mods-folder')[0];
  assert.equal(folder.getAttribute('data-group'), 'campaign-interactables');
  assert.equal(folder.all('mods-row')[0].getAttribute('data-key'), 'campaign-interactables\\doors');
  assert.deepEqual(keys(h), ['campaign-interactables\\doors', 'Arena Gates', 'Arena Gates (old)', 'bare-hands', 'Broken Barrels',
                             'Editor Grid', 'alex.r2-imp']);
  assert.deepEqual(h.ui.state().view.map(b => b.key), keys(h), 'the arrow keys walk the shown order');
  /* each row: the name and size, one glyph */
  const imp = rowOf(h, 'alex.r2-imp');
  assert.equal(imp.all('mods-row-name')[0].textContent, 'R2 Imp copy');
  assert.equal(imp.all('mods-row-sub')[0].textContent, '1.8 MB');
  assert.equal(imp.children.length, 2, 'glyph and text, nothing else');
  assert.ok(rows(h).every(n => n.all('mods-status-icon').length === 1));
  assert.equal(iconOf(imp), 'map', 'the open map uses it');
  assert.equal(iconOf(rowOf(h, 'campaign-interactables\\doors')), 'map');
  assert.equal(iconOf(rowOf(h, 'bare-hands')), 'map', 'the whole of a mod used in the whole map has the same glyph');
  assert.equal(iconOf(rowOf(h, 'Broken Barrels')), 'warn');
  assert.equal(rowOf(h, 'Broken Barrels').title, v4.mods.find(b => b.key === 'Broken Barrels').cause);
  assert.equal(iconOf(rowOf(h, 'Editor Grid')), 'ok');
  assert.equal(rowOf(h, 'Editor Grid').title, modsTab.READY_TEXT);
  assert.equal(iconOf(rowOf(h, 'Arena Gates')), 'ok', 'a mod the map does not use');
  /* the title names the state: the cause, then the state's head */
  assert.equal(rowOf(h, 'alex.r2-imp').title, 'Ready to use. In this map: 1 object placed (Imp (R2)).');
  assert.equal(rowOf(h, 'bare-hands').title, 'Ready to use. In this map: the whole mod.');
  assert.equal(rowOf(h, 'campaign-interactables\\doors').title, 'Ready to use. In this map: 2 objects placed (Blast door and Lift door).');
  assert.equal(rowOf(h, 'Arena Gates').title, modsTab.READY_TEXT, 'no state words for a mod outside the map');
  assert.equal(rowOf(h, 'alex.r2-imp').getAttribute('data-in-map'), 'true');
  assert.equal(rowOf(h, 'Arena Gates').getAttribute('data-in-map'), null);
  /* a group closes for the session; a closed group with a mod needing attention shows the triangle */
  areas[0].all('pf-folder-head')[0].click();
  assert.deepEqual(keys(h), []);
  assert.equal(h.nodes.modsList.children[0].all('mods-status-icon')[0].getAttribute('data-status'), 'warn');
  /* selecting a mod inside opens it again */
  h.ui.select('alex.r2-imp');
  assert.ok(rowOf(h, 'alex.r2-imp'));
  /* the list has no right-click menu */
  const e = h.nodes.modsList.dispatch('contextmenu', {pageX: 40, pageY: 90});
  assert.equal(e.defaultPrevented, false);
  assert.deepEqual(h.menus, []);
  assert.ok(!/contextmenu|env\.menu/.test(source), 'mods_tab.js has no menu code');
});
test('the filter matches name and folder; the attention filter', () => {
  const h = harness();
  shown(h);
  const f = h.nodes.modsFilter;
  f.value = 'arena';
  f.dispatch('input');
  assert.deepEqual(keys(h), ['Arena Gates', 'Arena Gates (old)']);
  f.value = 'interact';
  f.dispatch('input');
  assert.deepEqual(keys(h), ['campaign-interactables\\doors'], 'the organizing folder is matched too');
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
  assert.equal(keys(h).length, 7);
});
test('keyboard selection walks the shown rows', () => {
  const h = harness();
  shown(h);
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowDown'});
  assert.equal(h.ui.state().selectedKey, 'campaign-interactables\\doors');
  h.nodes.modsList.dispatch('keydown', {key: 'End'});
  assert.equal(h.ui.state().selectedKey, 'alex.r2-imp');
  h.nodes.modsList.dispatch('keydown', {key: 'ArrowUp'});
  assert.equal(h.ui.state().selectedKey, 'Editor Grid');
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
  /* the details pane's How mods work says it too, and how a map carries the mods it uses */
  assert.ok(h.nodes.modsDetail.textContent.indexOf(modsTab.REFRESH_TEXT) >= 0);
  const paras = h.nodes.modsDetail.all('mods-howto')[0].tags('p').map(n => n.textContent);
  assert.equal(paras[2], 'A map carries the mods it uses: the pieces of the objects you place, or the whole of a mod used in the whole map. A player who opens the map gets them with it.');
  assert.ok(!/Came with the map|Hell Guard/.test(h.nodes.modsDetail.textContent), 'no word of the places that were retired');
});

/* ---- the details pane ------------------------------------------------------------------------- */
test('details: description, what it adds, what it changes, what it borrows, console settings, files', () => {
  const h = live();
  h.ui.select('alex.r2-imp');
  const d = h.nodes.modsDetail;
  assert.equal(d.all('mods-detail-name')[0].textContent, 'R2 Imp copy');
  assert.equal(d.all('mods-detail-sentence')[0].textContent, modsTab.READY_TEXT);
  assert.deepEqual(d.all('mods-section').map(n => n.getAttribute('data-section')),
                   ['inmap', 'about', 'adds', 'changes', 'borrows', 'requirements', 'files', 'sharing']);
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
  assert.match(section(h, 'files').textContent, /^Files In its folder 13 files, 1\.8 MB$/);
  assert.ok(!/Show the files/.test(section(h, 'files').textContent + source), 'the Files section has no link; the Files button is the way in');
  assert.equal(section(h, 'files').tags('button').length, 0);
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
                   ['inmap', 'about', 'adds', 'requirements', 'files', 'sharing'], 'no derived facts for a mod that is not built');
  const im = inMap(h);
  assert.equal(im.state, 'out');
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Use in this map']);
  assert.equal(im.buttons[0].disabled, true, 'a mod that cannot be used is not added to a map');
  assert.equal(im.buttons[0].title, v4.mods.find(b => b.key === 'Broken Barrels').cause, 'its hover says why');
  im.buttons[0].click();
  assert.ok(!h.sent.some(m => m.action === 'mapAddMod'), 'a disabled button posts nothing');
});
/* ---- the In this map block: a mod and the open map, in one state, with the one action it allows ------ */
const IN_MAP = modsTab.MAP_STATE.inPrefix;
test('In this map: no map open says so for every mod, and offers nothing', () => {
  const closed = clone(v4);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  const h = live(closed);
  for (const b of modsTab.normalize(closed).mods) {
    h.ui.select(b.key);
    const im = inMap(h);
    if (b.key === 'Editor Grid') { assert.equal(im.state, 'editorOnly', 'an editor tweak is never in a map, open or not'); continue; }
    assert.equal(im.state, 'noMap', b.name);
    assert.equal(im.head, 'No map open');
    assert.deepEqual(im.buttons, [], b.name + ': nothing to press without a map');
    assert.equal(iconOf(im.box.all('mods-inmap-state')[0]), 'mapoff', 'the dimmed map glyph');
  }
  h.ui.select('alex.r2-imp');
  assert.equal(inMap(h).box.all('mods-line')[0].textContent, 'Open a map in the editor, then place its objects from the Toybox.');
  h.ui.select('bare-hands');
  assert.equal(inMap(h).box.all('mods-line')[0].textContent, 'Open a map in the editor to use it in the whole map.');
  /* the row has no in-this-map glyph and no state words either */
  assert.ok(rows(h).every(n => iconOf(n) !== 'map' && n.getAttribute('data-in-map') === null));
  /* the pure state agrees, for a model with no map at all, and for a map that is not open */
  assert.equal(modsTab.mapState(null, {editorRows: 1, modules: 0, facts: null}).state, 'noMap');
  assert.equal(modsTab.mapState({open: false, mods: []}, {editorRows: 0, modules: 0, facts: null}).state, 'noMap');
});
test('In this map: out of the map, a mod with objects to place only says to place them', () => {
  const h = live();
  h.ui.select('Arena Gates');
  const im = inMap(h);
  assert.equal(im.state, 'out');
  assert.equal(im.head, 'Not in this map');
  assert.equal(im.box.all('mods-section-head')[0].textContent, 'This map');
  assert.equal(iconOf(im.box.all('mods-inmap-state')[0]), 'mapoff');
  assert.deepEqual(im.box.all('mods-line').map(n => n.textContent), ['Place its objects from the Toybox; the map then carries their pieces.']);
  assert.deepEqual(im.buttons, [], 'it has Toybox rows to place: no Use in this map');
  assert.equal(im.box.all('mods-inmap-actions').length, 0, 'no actions row at all');
  /* modules and demons for the encounter list are things to place too */
  for (const [what, edit] of [['modules', b => { b.modules = 2; }],
                              ['encounters', b => { b.facts = {adds: 0, registries: {conductor: ['mods/barehand/imp']}}; }]]) {
    const d = withMapMods(v4.map.mods.filter(r => r.salt !== 'barehand'));
    edit(d.mods.find(b => b.key === 'bare-hands'));
    const g = live(d);
    g.ui.select('bare-hands');
    assert.equal(inMap(g).state, 'out', what);
    assert.deepEqual(inMap(g).buttons, [], what + ' are placed, not used as a whole');
    assert.equal(modsTab.placeable(modsTab.normalize(d).mods.find(b => b.key === 'bare-hands')), true);
  }
  assert.equal(modsTab.placeable({editorRows: 0, modules: 0, facts: null}), false);
  assert.equal(modsTab.placeable(null), false);
});
test('In this map: Use in this map only for a mod with nothing to place; it sends mapAddMod and the mod is then used as a whole', () => {
  const d = withMapMods(v4.map.mods.filter(r => r.salt !== 'barehand'));
  const h = live(d);
  h.ui.select('bare-hands');
  const im = inMap(h);
  assert.equal(im.state, 'out');
  assert.deepEqual(im.box.all('mods-line').map(n => n.textContent), ['It has nothing to place, so a map uses it as a whole.']);
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Use in this map']);
  assert.equal(im.buttons[0].disabled, false);
  assert.equal(im.buttons[0].title, 'Use this mod in the whole open map; the map carries all of it when you save');
  assert.equal(im.buttons[0].className, 'btn');
  im.buttons[0].click();
  assert.deepEqual(h.sent.filter(m => m.cmd === 'modsAction').slice(-1)[0], {cmd: 'modsAction', action: 'mapAddMod', arg: 'bare-hands'});
  assert.equal(h.toasts.pop().text, 'Bare Hands is now used by the whole of Hell Guard Arena; save the map to keep it.');
  const after = inMap(h);
  assert.equal(after.state, 'whole', 'the document drives it');
  assert.equal(after.head, 'In this map: the whole mod');
  assert.deepEqual(after.buttons.map(b => b.textContent), ['Remove from map']);
  assert.equal(iconOf(rowOf(h, 'bare-hands')), 'map');
  const busy = harness();
  shown(busy, d);
  busy.ui.select('bare-hands');
  assert.equal(inMap(busy).buttons[0].disabled, false);
  busy.nodes.modsRefreshBtn.click();
  assert.equal(busy.ui.state().busy, 'rescan');
  assert.equal(inMap(busy).buttons[0].disabled, true, 'held while another action runs');
});
test('In this map: a mod used as a whole, or whose objects the map places, can be removed from the map; the dry run drives it', () => {
  const h = live();
  h.ui.select('bare-hands');
  let im = inMap(h);
  assert.equal(im.state, 'whole');
  assert.equal(im.head, IN_MAP + 'the whole mod');
  assert.equal(iconOf(im.box.all('mods-inmap-state')[0]), 'map', 'the map glyph in full');
  assert.deepEqual(im.box.all('mods-line').map(n => n.textContent), ['The map carries all of it.']);
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Remove from map']);
  assert.equal(im.buttons[0].className, 'btn danger');
  assert.equal(im.buttons[0].title, 'Remove Bare Hands and everything that uses it from this map');
  im.buttons[0].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveModPreview', arg: 'bare-hands'});
  assert.equal(h.ui.state().modal.page, 'confirm');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Remove Bare Hands from Hell Guard Arena?');
  assert.deepEqual(modalButtons(h).map(b => b.textContent), ['Cancel', 'Remove from map']);
  modalButtons(h)[0].click();
  assert.equal(h.ui.state().modal.open, false, 'started from the details pane: Cancel closes the dialog');
  h.ui.select('bare-hands');
  inMap(h).buttons[0].click();
  modalButtons(h)[1].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapRemoveMod', arg: 'bare-hands'});
  assert.equal(h.ui.state().modal.open, false, 'started from the details pane: the dialog closes');
  assert.match(h.toasts.pop().text, /^Removed Bare Hands from Hell Guard Arena: 0 objects deleted\./);
  im = inMap(h);
  assert.equal(im.state, 'out');
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Use in this map'], 'nothing to place: it can be used again');
  /* the objects the map places */
  h.ui.select('alex.r2-imp');
  im = inMap(h);
  assert.equal(im.state, 'objects');
  assert.equal(im.head, IN_MAP + '1 object placed (Imp (R2))');
  assert.deepEqual(im.box.all('mods-line').map(n => n.textContent), ["The map carries these objects' pieces."]);
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Remove from map']);
  h.ui.select('campaign-interactables\\doors');
  assert.equal(inMap(h).head, IN_MAP + '2 objects placed (Blast door and Lift door)');
  /* a mod used as a whole that also has objects placed says both */
  const both = withMapMods(v4.map.mods.map(r => r.salt === 'doorsx8q' ? Object.assign({}, r, {attached: true}) : r));
  const g = live(both);
  g.ui.select('campaign-interactables\\doors');
  assert.equal(inMap(g).state, 'whole');
  assert.equal(inMap(g).head, IN_MAP + 'the whole mod, and 2 objects placed (Blast door and Lift door)');
});
test('In this map: a mod only other objects need has no action, and goes when they go', () => {
  const d = clone(v4);
  d.map.mods.push(mapRow({key: 'Arena Gates', salt: 'gatesk2m', name: 'Arena Gates', size: 412000}));
  const h = live(d);
  h.ui.select('Arena Gates');
  const im = inMap(h);
  assert.equal(im.state, 'needed');
  assert.equal(im.head, IN_MAP + 'needed by other objects in the map');
  assert.deepEqual(im.box.all('mods-line').map(n => n.textContent), ['The map carries the pieces those objects need; it goes when they go.']);
  assert.deepEqual(im.buttons, [], 'neither Use in this map nor Remove from map');
  assert.equal(iconOf(im.box.all('mods-inmap-state')[0]), 'map');
  assert.equal(iconOf(rowOf(h, 'Arena Gates')), 'map', 'the row shows the map glyph for it too');
  assert.equal(rowOf(h, 'Arena Gates').title, 'Ready to use. In this map: needed by other objects in the map.');
  /* the placed pieces only: modules count as objects */
  const mods = withMapMods([mapRow({key: 'Arena Gates', salt: 'gatesk2m', placedModules: 2, viaNeeds: false})]);
  const m = live(mods);
  m.ui.select('Arena Gates');
  assert.equal(inMap(m).state, 'objects');
  assert.equal(inMap(m).head, IN_MAP + '2 objects placed');
});
test('In this map: an editor tweak is never in a map, whatever the map lists', () => {
  const d = clone(v4);
  d.map.mods.push(mapRow({key: 'Editor Grid', salt: 'edgrid4t', name: 'Editor Grid', attached: true}));
  for (const data of [v4, d]) {
    const h = live(data);
    h.ui.select('Editor Grid');
    const im = inMap(h);
    assert.equal(im.state, 'editorOnly');
    assert.equal(im.head, 'Not used by maps');
    assert.deepEqual(im.box.all('mods-line').map(n => n.textContent), ["It changes only this computer's editor; a map never carries it."]);
    assert.deepEqual(im.buttons, []);
    assert.equal(iconOf(im.box.all('mods-inmap-state')[0]), 'mapoff');
    assert.equal(iconOf(rowOf(h, 'Editor Grid')), 'ok', 'no map glyph on its row');
  }
});
test('In this map: a mod is matched to the map by its salt without case, then by its folder; two folders of one salt both show it', () => {
  const imp = modsTab.normalize(v4).mods.find(b => b.key === 'alex.r2-imp');
  const map = modsTab.normalize(v4).map;
  assert.equal(modsTab.mapEntry(map, imp).salt, 'alexrimp');
  assert.equal(modsTab.mapEntry(map, Object.assign({}, imp, {salt: 'ALEXRIMP', key: 'another folder'})).salt, 'alexrimp', 'the salt, not the folder, and without case');
  assert.equal(modsTab.mapEntry(map, Object.assign({}, imp, {salt: 'other', key: 'alex.r2-imp'})).salt, 'alexrimp', 'the folder when no salt matches');
  assert.equal(modsTab.mapEntry(map, Object.assign({}, imp, {salt: 'other', key: 'other'})), null);
  assert.equal(modsTab.mapEntry(map, Object.assign({}, imp, {salt: '', key: ''})), null, 'an empty salt and an empty folder match nothing');
  assert.equal(modsTab.mapEntry({open: false, mods: map.mods}, imp), null, 'no entry without an open map');
  assert.equal(modsTab.mapEntry(null, imp), null);
  assert.equal(modsTab.mapEntry(map, null), null);
  /* the map's own entry for Spiked Walls has no folder: the salt finds it */
  assert.equal(modsTab.mapEntry(map, {salt: 'JOGUARDS', key: ''}).name, 'Spiked Walls');
  /* two folders of one salt: the player's own and another build of it */
  const d = clone(v4);
  const twin = Object.assign(clone(d.mods.find(b => b.key === 'alex.r2-imp')), {key: 'older\\imp copy', folder: 'older\\imp copy', group: 'older'});
  d.mods.push(twin);
  const h = live(d);
  assert.equal(h.nodes.modsBadge.textContent, '8');
  for (const key of ['alex.r2-imp', 'older\\imp copy']) {
    assert.equal(iconOf(rowOf(h, key)), 'map', key);
    h.ui.select(key);
    assert.equal(inMap(h).state, 'objects', key);
    assert.deepEqual(inMap(h).buttons.map(b => b.textContent), ['Remove from map']);
  }
  /* the folder fallback: an entry without a salt names the mod's folder */
  const byKey = withMapMods([mapRow({key: 'Arena Gates', salt: '', name: 'Arena Gates', attached: true})]);
  const k = live(byKey);
  k.ui.select('Arena Gates');
  assert.equal(inMap(k).state, 'whole');
  /* the tab's one question for the Prefabs tab: a state by namespace, without case */
  const ui = h.ui;
  assert.equal(ui.mapStateOf('alexrimp').state, 'objects');
  assert.equal(ui.mapStateOf('ALEXRIMP').state, 'objects');
  assert.equal(ui.mapStateOf('barehand').state, 'whole');
  assert.equal(ui.mapStateOf('gatesk2m').state, 'out');
  assert.equal(ui.mapStateOf('edgrid4t').state, 'editorOnly');
  assert.equal(ui.mapStateOf('joguards').state, 'objects', 'a mod the map lists and the player has not installed');
  assert.equal(harness().ui.mapStateOf('alexrimp'), null, 'nothing before the first document');
  const closed = clone(v4);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  assert.equal(live(closed).ui.mapStateOf('alexrimp').state, 'noMap');
});
test('In this map: the block comes right after the details sentence, in the same place for every mod', () => {
  const data = clone(v4);
  data.mods.find(b => b.key === 'campaign-interactables\\doors').restartBanks = ['doors_sfx.bnk'];
  data.map.mods.push(mapRow({key: 'Arena Gates', salt: 'gatesk2m', name: 'Arena Gates'}));
  const closed = clone(data);
  closed.map = {entities: -1, names: 0, rows: [], placed: []};
  for (const d of [data, closed]) {
    const h = live(d);
    h.ui.select('');
    assert.equal(h.nodes.modsDetail.all('mods-inmap').length, 0, 'nothing selected: How mods work, no block');
    for (const b of modsTab.normalize(d).mods) {
      h.ui.select(b.key);
      const kids = h.nodes.modsDetail.children;
      const at = kids.findIndex(n => /\bmods-detail-sentence\b/.test(n.className));
      assert.ok(at > 0, b.name);
      assert.equal(kids[0].className, 'mods-detail-head');
      /* the sentence, a sound bank's restart note when the mod has one, then the block */
      const note = b.restartBanks.length;
      assert.equal(kids[at + 1 + note].id, 'modsInMap', b.name + ': right after the sentence');
      assert.equal(kids[at + 1 + note].className, 'mods-section mods-inmap');
      assert.equal(kids[at + 1 + note].getAttribute('data-section'), 'inmap');
      assert.ok(kids[at + 1 + note].getAttribute('data-state'), b.name);
      assert.equal(kids.filter(n => n.id === 'modsInMap').length, 1, 'one block');
      assert.equal(h.nodes.modsDetail.all('mods-inmap').length, 1);
      /* its parts: the head, the state line (glyph and head), the muted line */
      const box = kids[at + 1 + note];
      assert.equal(box.children[0].className, 'mods-section-head');
      assert.equal(box.children[0].textContent, 'This map');
      assert.equal(box.children[1].className, 'mods-inmap-state');
      assert.equal(box.children[1].children.length, 2);
      assert.equal(box.children[1].children[0].className.split(' ')[0], 'mods-status-icon');
      assert.equal(box.children[1].children[1].className, 'mods-inmap-head');
      assert.equal(box.children[2].className, 'mods-line muted');
      /* the same-files hint, an informational note, comes after it */
      if (b.sameFilesAs) assert.match(kids[at + 2 + note].className, /mods-same/);
    }
  }
  assert.ok(!/modsUseBtn/.test(source), 'no Use button of the head any more');
});
test('In this map: a copy of the mod the map holds as published says so; another installed version offers Update', () => {
  const d = clone(v4);
  d.map.mods[0].pinned = true;
  const h = live(d);
  h.ui.select('alex.r2-imp');
  let im = inMap(h);
  assert.deepEqual(im.box.all('mods-line').map(n => n.textContent),
                   ["The map carries these objects' pieces.", 'The map holds its own published copy of this mod.']);
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Remove from map'], 'no update line, no Update button');
  d.map.mods[0].update = 'This map uses another version of R2 Imp copy than the one you have.';
  const u = live(d);
  u.ui.select('alex.r2-imp');
  im = inMap(u);
  assert.ok(im.box.all('mods-line').map(n => n.textContent).includes(d.map.mods[0].update), 'the update line');
  assert.deepEqual(im.buttons.map(b => b.textContent), ['Remove from map', 'Update']);
  assert.equal(im.buttons[1].title, 'Use the version of R2 Imp copy you have installed for every object of it in this map');
  im.buttons[1].click();
  assert.deepEqual(u.sent.filter(m => m.cmd === 'modsAction').slice(-1)[0], {cmd: 'modsAction', action: 'mapUpdate', arg: 'alexrimp'});
  /* a mod that is not used has neither line */
  u.ui.select('Arena Gates');
  assert.deepEqual(inMap(u).buttons, []);
  assert.ok(!inMap(u).box.textContent.includes('published copy'));
});
test('the open map follows the document: placing, deleting and undoing move every glyph and block at once', () => {
  const h = live();
  h.ui.select('alex.r2-imp');
  assert.equal(inMap(h).state, 'objects');
  const next = clone(v4);
  next.map.mods = next.map.mods.filter(r => r.salt !== 'alexrimp');
  h.ui.onMessage({kind: 'mods', data: next});
  assert.equal(inMap(h).state, 'out', 'the last object deleted: the block, selected, follows');
  assert.equal(iconOf(rowOf(h, 'alex.r2-imp')), 'ok');
  h.ui.onMessage({kind: 'mods', data: v4});
  assert.equal(inMap(h).state, 'objects');
  assert.equal(iconOf(rowOf(h, 'alex.r2-imp')), 'map');
});
test('an author never needs the salt: no Copy mod id, and the folder rule said where they look', () => {
  const h = harness();
  shown(h);
  h.ui.select('alex.r2-imp');
  assert.equal(h.nodes.modsDetail.all('mods-copy-id').length, 0);
  assert.ok(!/Copy mod id|mod id/i.test(source), 'no Copy mod id and no "mod id" anywhere in the page code');
  assert.equal(modsTab.folderRuleText('Crate Pack'),
      'Put each new file in the folder for what it is (materials, images, meshes, animations, effects, entities, sounds, scripts), and in your files name it by that folder and its path, as in materials/crate.');
  h.ui.select('');
  assert.ok(h.nodes.modsDetail.textContent.indexOf(modsTab.folderRuleText('')) >= 0, 'How mods work says it too');
});
test('details: a folder of editor tweaks only has no map action', () => {
  const h = live();
  h.ui.select('Editor Grid');
  assert.deepEqual(inMap(h).buttons, []);
  assert.ok(section(h, 'requirements').textContent.includes(modsTab.EDITOR_ONLY_NOTE));
  assert.match(section(h, 'changes').textContent, /Tweaks 1 editor file on this computer only/);
  h.ui.select('alex.r2-imp');
  assert.deepEqual(inMap(h).buttons.map(b => b.textContent), ['Remove from map']);
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
  /* Install is one plain button: the host's one Open dialog takes mod ZIPs and a folder's mod.json; no menu */
  h.nodes.modsInstallBtn.click();
  assert.deepEqual(h.menus, [], 'no menu opens');
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'install'});
  assert.equal(h.ui.state().busy, 'install');
  assert.equal(h.nodes.modsStatus.textContent, 'Installing and building...');
  h.ui.onMessage({kind: 'modsActionResult', action: 'install', ok: false, data: {cancelled: true}});
  assert.equal(h.ui.state().busy, '');
  assert.equal(h.nodes.modsStatus.textContent, '', 'a cancelled picker says nothing');
  /* the foot's path is the button for the mods folder, with the mouse and with the keys */
  assert.equal(h.nodes.modsRoot.getAttribute('role'), 'button');
  assert.equal(h.nodes.modsRoot.textContent, v4.root);
  assert.ok(h.nodes.modsRoot.title.indexOf(v4.root) >= 0, 'its hover names the folder');
  h.nodes.modsRoot.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'openFolder'});
  assert.equal(h.ui.state().busy, '', 'opening a folder never holds the page');
  h.sent.length = 0;
  for (const key of ['Enter', ' ']) {
    const e = h.nodes.modsRoot.dispatch('keydown', {key});
    assert.equal(e.defaultPrevented, true, JSON.stringify(key));
  }
  assert.equal(h.nodes.modsRoot.dispatch('keydown', {key: 'a'}).defaultPrevented, false);
  assert.deepEqual(h.sent, [{cmd: 'modsAction', action: 'openFolder'}, {cmd: 'modsAction', action: 'openFolder'}], 'Enter and Space, no other key');
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
  h.ui.onMessage({kind: 'modsActionResult', action: 'newMod', applyPending: true, ok: true, message: 'Created Late in your mods folder, as Late.',
                  data: {key: 'Late'}});
  assert.equal(h.toasts.pop().text, 'Created Late in your mods folder, as Late. ' + modsTab.APPLY_PENDING_TEXT);
});
test('the map modal: the open map\'s details and mods, in plain words', () => {
  const h = live();
  const ov = h.nodes.modsMapModal;
  h.nodes.modsMapBtn.click();
  assert.equal(ov.style.display, 'flex');
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Hell Guard Arena');
  const t = modalText(h);
  for (const s of ['Name Hell Guard Arena', 'Published Yes, 2026-', 'Mods 4 mods, 5.4 MB'])
    assert.ok(t.indexOf(s) >= 0, s);
  assert.ok(t.indexOf('Saved map') < 0, 'no saved-map id');
  assert.ok(t.indexOf('Its mods') < 0, 'a published map whose content is claimed has no Its mods line');
  assert.ok(t.indexOf('for a player to download') < 0, 'the size is a download only when every row is the map\'s own published copy');
  assert.ok(t.indexOf('Missing for players') < 0);
  const mr = h.nodes.modsMapHost.all('mods-map-row');
  assert.equal(mr.length, 4);
  assert.match(mr[0].textContent, /^R2 Imp copy 1\.8 MB 1 object placed \(Imp \(R2\)\) Also used by timeline: Wave 2: spawn the imps, wire: Relay: all imps dead Remove from map$/);
  assert.match(mr[1].textContent, /^Doors 3\.6 MB 2 objects placed \(Blast door and Lift door\) Remove from map$/);
  assert.equal(iconOf(mr[0]), 'map', 'installed rows show the map glyph');
  assert.equal(iconOf(mr[1]), 'map');
  assert.equal(iconOf(mr[2]), 'warn');
  assert.match(mr[2].textContent, /^Spiked Walls 51 KB 1 object placed \(Spiked wall\); not installed on this computer Remove from map$/);
  assert.equal(iconOf(mr[3]), 'map');
  assert.equal(mr[3].getAttribute('data-attached'), 'true');
  assert.match(mr[3].textContent, /^Bare Hands 4\.1 KB The whole mod Remove from map$/);
  assert.equal(mr[0].title, 'In this map: 1 object placed (Imp (R2)).');
  assert.equal(mr[2].title, 'not installed on this computer');
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
  assert.equal(inMap((c.ui.select('alex.r2-imp'), c)).state, 'noMap');
});
test('the map modal: what a map uses of a mod, the published copy, and the Mods and Its mods fields', () => {
  /* a row only other objects need has no Remove from map; a mod used as a whole and with objects says both */
  const d = withMapMods([
    mapRow({key: 'Arena Gates', salt: 'gatesk2m', name: 'Arena Gates', size: 412000}),
    mapRow({key: 'bare-hands', salt: 'barehand', name: 'Bare Hands', size: 4210, attached: true, objects: ['Gloves'], placedCount: 1, viaNeeds: false}),
    mapRow({key: 'alex.r2-imp', salt: 'alexrimp', name: 'R2 Imp copy', size: 1843230, placedModules: 3, viaNeeds: false, pinned: true})]);
  const h = live(d);
  h.nodes.modsMapBtn.click();
  const mr = h.nodes.modsMapHost.all('mods-map-row');
  assert.equal(mr.length, 3);
  assert.match(mr[0].textContent, /^Arena Gates 402 KB Needed by other objects in the map$/);
  assert.equal(mr[0].tags('button').length, 0, 'nothing of it to remove from the map by itself');
  assert.match(mr[1].textContent, /^Bare Hands 4\.1 KB The whole mod, and 1 object placed \(Gloves\) Remove from map$/);
  assert.match(mr[2].textContent, /^R2 Imp copy 1\.8 MB 3 objects placed The map holds its own published copy of this mod\. Remove from map$/);
  assert.ok(!/MAP_LEVEL|Used by the whole map, nothing placed/.test(modalText(h)));
  /* Mods: "for a player to download" only when every row is a published copy */
  assert.match(modalText(h), /Mods 3 mods, 2\.2 MB(?! for a player)/);
  const all = withMapMods(v4.map.mods.map(r => Object.assign({}, r, {pinned: true})));
  const p = live(all);
  p.nodes.modsMapBtn.click();
  assert.match(modalText(p), /Mods 4 mods, 5\.4 MB for a player to download/);
  /* Its mods: a published map whose content is not yet claimed; not for a claimed one or a map not published */
  const unclaimed = clone(v4);
  unclaimed.map.uploaded = {claimed: false, at: 0};
  const u = live(unclaimed);
  u.nodes.modsMapBtn.click();
  assert.match(modalText(u), /Its mods Snapmap\+ sends its mod content to the mod store the next time DOOM starts\./);
  const draft = clone(v4);
  delete draft.map.published;
  delete draft.map.uploaded;
  const dr = live(draft);
  dr.nodes.modsMapBtn.click();
  assert.ok(modalText(dr).indexOf('Its mods') < 0, 'a map that is not published sent nothing');
  assert.ok(modalText(dr).indexOf('Published') < 0);
  /* a map that uses no mods */
  const none = withMapMods([]);
  none.map.vanilla = true;
  const n = live(none);
  n.nodes.modsMapBtn.click();
  assert.match(modalText(n), /Mods 0 mods(?!,)/);
  assert.match(modalText(n), /This map uses no mods now; it plays without Snapmap\+\./);
  /* the files players lacked are not a section of the window any more, with or without the data */
  const asked = clone(v4);
  asked.map.asked = [{mod: 'R2 Imp copy', request: 'generated/image/mods/alexrimp/glow.bimage', by: ''}];
  const a = live(asked);
  a.nodes.modsMapBtn.click();
  assert.equal(section(a, 'asked', a.nodes.modsMapHost), undefined);
  assert.ok(modalText(a).indexOf('Missing for players') < 0 && modalText(a).indexOf('glow.bimage') < 0);
  assert.ok(!/Missing for players/.test(source));
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
  assert.equal(h.nodes.modsMapHost.all('mods-map-row').length, 3);
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
test('Use in this map and back, from the block; the backend answers a mod that is used or not installed', () => {
  const h = live(withMapMods(v4.map.mods.filter(r => r.salt !== 'barehand')));
  h.ui.select('bare-hands');
  inMap(h).buttons[0].click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'mapAddMod', arg: 'bare-hands'});
  assert.match(h.toasts.pop().text, /^Bare Hands is now used by the whole of Hell Guard Arena; save the map to keep it\.$/);
  assert.deepEqual(inMap(h).buttons.map(b => b.textContent), ['Remove from map']);
  inMap(h).buttons[0].click();
  assert.equal(h.ui.state().modal.page, 'confirm');
  modalButtons(h)[1].click();
  assert.equal(h.ui.state().modal.open, false, 'started from the details pane: the dialog closes');
  assert.deepEqual(inMap(h).buttons.map(b => b.textContent), ['Use in this map']);
  h.ui.act('mapAddMod', 'no such mod');
  assert.deepEqual(h.toasts.pop(), {text: 'no such mod is not installed.', kind: 'err'});
  h.ui.act('mapAddMod', 'bare-hands');
  h.ui.act('mapAddMod', 'bare-hands');
  assert.equal(h.toasts.pop().text, 'Bare Hands is already used by the whole of Hell Guard Arena.');
});
test('Uninstall: the confirmation lists what it does; the folder goes to the Recycle Bin', () => {
  const h = live();
  h.ui.select('alex.r2-imp');
  h.nodes.modsUninstallBtn.click();
  assert.deepEqual(h.sent.slice(-1)[0], {cmd: 'modsAction', action: 'uninstallPreview', arg: 'alex.r2-imp'});
  assert.equal(h.nodes.modsMapHost.all('modal-title')[0].textContent, 'Uninstall R2 Imp copy?');
  const items = h.nodes.modsMapHost.tags('li').map(n => n.textContent);
  assert.ok(items.indexOf('Removes 2 Toybox rows from the editor (Imp (R2), Imp (R2, caged)).') >= 0);
  assert.equal(items[items.length - 1], 'Moves its folder, alex.r2-imp, to the Windows Recycle Bin.');
  assert.equal(h.nodes.modsMapHost.all('mods-scope')[0].textContent, modsTab.SCOPE.uninstallUsed);
  modalButtons(h)[1].click();
  assert.equal(rowOf(h, 'alex.r2-imp'), undefined);
  assert.equal(h.ui.state().selectedKey, '');
  assert.equal(h.nodes.modsBadge.textContent, '6');
  /* the consequence lines of the dry run's fields */
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall',
    install: {folder: 'older\\Imp copy', savedMaps: 0, keptBuild: 'Imp copy'}})).map(c => c.text),
    ['Moves its folder, older\\Imp copy, to the Windows Recycle Bin.',
     'Another folder of this mod, Imp copy, stays installed; maps that use the mod open with it.']);
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall', install: {folder: 'a', savedMaps: 3}})).map(c => c.text),
    ['Moves its folder, a, to the Windows Recycle Bin.',
     '3 of your saved maps use it; they will open without its objects until you restore it from the Recycle Bin.']);
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'uninstall', install: {folder: 'a', savedMaps: 1}})).map(c => c.text),
    ['Moves its folder, a, to the Windows Recycle Bin.',
     '1 of your saved maps uses it; it will open without its objects until you restore it from the Recycle Bin.']);
  /* a published map: its content in the mod store changes only at the next publish, in the product's words */
  assert.deepEqual(modsTab.consequenceLines(modsTab.normPreview({action: 'mapRemoveModPreview',
    map: {name: 'Arena', entities: [], entityCount: 0, references: [], manifestRow: false, published: true}})).map(c => c.text),
    ["The map's content in the mod store is unaffected until you publish the map again."]);
});

/* ---- New mod and the Files view --------------------------------------------------------------- */
test('New mod: a name, the folder made with its example, Refresh named as the way to pick up edits', () => {
  const h = harness({store: standIn(v4)});
  h.ui.show();
  h.nodes.modsNewBtn.click();
  assert.equal(h.nodes.modsNewModal.style.display, 'flex');
  assert.equal(h.nodes.modsNewNote.textContent, "Snapmap+ makes a folder for it in your mods folder with an example crate you can place right away.");
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
  /* the list has no right-click entry any more: New mod is the header's primary button and the empty list's */
  const e = h.nodes.modsList.dispatch('contextmenu', {pageX: 40, pageY: 90});
  assert.equal(e.defaultPrevented, false);
  assert.deepEqual(h.menus, []);
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
  assert.deepEqual(store({cmd: 'modsAction', action: 'uninstallPreview', arg: 'alex.r2-imp'})[0].data.preview.editorRows,
    ['Imp (R2)', 'Imp (R2, caged)']);
  assert.equal(v4.mods.length, 7, 'the fixture is untouched');
  /* content: the result alone, the lines with their tokens in rows; addToMine takes a token */
  const kept = standIn(v4, null, {storage: 'Map content: 1.3 GB for 14 maps.', lines: ['Eternal Demons: Hell Guards'], rows: ['maps\\Eternal Demons\\Hell Guards']});
  const c = kept({cmd: 'modsAction', action: 'content'});
  assert.deepEqual(c.map(m => m.kind), ['modsActionResult'], 'no fresh document follows a read of the content');
  assert.deepEqual([c[0].data.lines, c[0].data.rows], [['Eternal Demons: Hell Guards'], ['maps\\Eternal Demons\\Hell Guards']]);
  assert.equal(kept({cmd: 'modsAction', action: 'addToMine', arg: 'nope'})[0].ok, false);
  const added = kept({cmd: 'modsAction', action: 'addToMine', arg: 'maps\\Eternal Demons\\Hell Guards'});
  assert.equal(added[0].data.key, 'Hell Guards');
  assert.ok(added[1].data.mods.some(b => b.key === 'Hell Guards' && b.modules === 0 && !('place' in b)));
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
  assert.equal(buttons, 5, 'attention, This map, Refresh, Install, New mod; the folder opens from the foot');
  assert.ok(2 * inset + headGap + buttons * button + (buttons - 1) * gap <= min, 'five buttons fit in a ' + min + ' px pane');
});
/* ---- mod scripts ------------------------------------------------------------------------------ */
const DOORS = 'campaign-interactables\\doors';
const STOP_LINE = 'Script stopped: Doors, scripts\\door.lua line 5: "open_door" is nothing here. Check the name you pass.';
function timers() {
  const t = {live: [], every: (fn, ms) => { const h = {fn, ms}; t.live.push(h); return h; },
             cancel: h => { t.live = t.live.filter(x => x !== h); }, tick: () => t.live.slice().forEach(h => h.fn())};
  return t;
}
test('scripts in words: what they drive and when, the Script nodes they answer and fire, names set while running', () => {
  const m = modsTab.normalize(v4);
  const doors = m.mods.find(b => b.key === DOORS);
  assert.deepEqual(modsTab.scriptLines(doors.scripts).map(l => l.text), [
    'Drives props/door_switch when it spawns and every frame.',
    'Answers the Script node named "switch_used": a wire into one runs the script.',
    'Fires the Script node named "open_door"; wire what should happen to its On Fired.']);
  assert.deepEqual(modsTab.scriptLines({files: 3, entities: [{name: 'ai/hell_guard', events: ['death', 'think', 'damage']}], inputs: [], outputs: [], dynamic: 2}).map(l => l.text), [
    'Drives ai/hell_guard every frame, when it takes damage and when it dies.', modsTab.SCRIPT_DYNAMIC_TEXT]);
  assert.deepEqual(modsTab.scriptLines({files: 1, entities: [], inputs: ['a', 'b', 'c'], outputs: [], dynamic: 0}).map(l => l.text),
                   ['Answers the Script nodes named "a", "b" and "c": a wire into one runs the script.']);
  assert.match(modsTab.scriptLines({files: 1, entities: [], inputs: [], outputs: [], dynamic: 0})[0].text, /run when the map starts/);
  assert.equal(m.mods.find(b => b.key === 'alex.r2-imp').scripts, null, 'a mod without scripts has none');
  assert.equal(modsTab.subText(doors), '3.6 MB, runs scripts');
  assert.equal(modsTab.scriptRunText(doors.scriptRun), 'They ran in the last Play.');
  assert.equal(modsTab.scriptRunText(null), modsTab.SCRIPT_RUN_TEXT.none);
  assert.equal(modsTab.scriptRunText({status: 'stopped', stop: STOP_LINE}), STOP_LINE);
});
test('a stopped script is the row\'s state until the next Play; it is not attention', () => {
  const data = clone(v4);
  data.mods.find(b => b.key === DOORS).scriptRun = {status: 'stopped', stop: STOP_LINE, lines: 3};
  const h = live(data);
  const row = rowOf(h, DOORS);
  assert.equal(iconOf(row), 'stop');
  assert.equal(row.title, STOP_LINE + ' In this map: 2 objects placed (Blast door and Lift door).', 'the stop line, then what the map uses of it');
  assert.match(row.textContent, /runs scripts/);
  h.ui.select(DOORS);
  assert.equal(h.nodes.modsDetail.all('mods-detail-sentence')[0].textContent, STOP_LINE);
  assert.deepEqual(inMap(h).buttons.map(b => [b.textContent, b.disabled]), [['Remove from map', false]], 'a mod whose script stopped can still be removed from a map');
  assert.equal(modsTab.statusInfo(modsTab.normalize(data).mods.find(b => b.key === DOORS)).tone, 'stop');
  h.ui.setAttentionOnly(true);
  assert.ok(!keys(h).includes(DOORS), 'the attention filter keeps the mods that cannot be used');
});
test('details: the Scripts section and the script log, its Copy and its note', () => {
  const t = timers(), copied = [];
  const h = harness({store: standIn(v4), every: t.every, cancel: t.cancel, copy: x => copied.push(x)});
  h.ui.show();
  h.ui.select(DOORS);
  const sc = section(h, 'scripts');
  assert.ok(sc, 'a Scripts section');
  assert.match(sc.textContent, /Files 2 files/);
  assert.match(sc.textContent, /Drives props\/door_switch when it spawns and every frame\./);
  assert.match(sc.textContent, /Answers the Script node named "switch_used"/);
  const log = section(h, 'scriptLog');
  assert.match(log.textContent, /They ran in the last Play\./);
  assert.match(log.textContent, /Reading the log\.\.\./);
  assert.ok(log.textContent.indexOf(modsTab.SCRIPT_LOG_NOTE) >= 0);
  const ask = h.sent.filter(m => m.cmd === 'modsScripts');
  assert.ok(ask.length >= 1 && ask[ask.length - 1].salt === 'doorsx8q', 'the selected mod\'s log is asked for');
  /* the answer: the log in place, and the state */
  h.ui.onMessage({kind: 'modsScripts', data: {mods: [{salt: 'doorsx8q', status: 'running', stop: '', lines: 2}], salt: 'doorsx8q',
                                             total: 2, full: false, log: ['door: switch 1 of 3', 'door: switch 2 of 3']}});
  const log2 = section(h, 'scriptLog');
  assert.match(log2.textContent, /door: switch 1 of 3 door: switch 2 of 3/);
  assert.match(log2.textContent, /Running in this Play\./);
  assert.doesNotMatch(log2.textContent, /The newest/);
  /* Copy log asks the host for the whole log and copies what comes back */
  log2.tags('button').find(b => b.textContent === 'Copy log').click();
  assert.deepEqual(h.sent[h.sent.length - 1], {cmd: 'modsScripts', salt: 'doorsx8q', full: 1});
  assert.deepEqual(copied, [], 'nothing until the whole log arrives');
  h.ui.onMessage({kind: 'modsScripts', data: {mods: [{salt: 'doorsx8q', status: 'running', stop: '', lines: 2}], salt: 'doorsx8q',
                                             total: 2, full: true, log: ['door: switch 1 of 3', 'door: switch 2 of 3']}});
  assert.deepEqual(copied, ['door: switch 1 of 3\ndoor: switch 2 of 3\n']);
  assert.equal(h.toasts[h.toasts.length - 1].text, 'Copied 2 lines of the script log.');
  /* a long log: the window shows the newest lines and says so; Copy log copies every one */
  {
    const tail = [], all = [];
    for (let i = 1; i <= 100000; i++) all.push('line ' + i);
    for (let i = 99001; i <= 100000; i++) tail.push('line ' + i);
    h.ui.onMessage({kind: 'modsScripts', data: {mods: [{salt: 'doorsx8q', status: 'running', stop: '', lines: 100000}],
                                               salt: 'doorsx8q', total: 100000, full: false, log: tail}});
    assert.match(section(h, 'scriptLog').textContent, /The newest 1000 lines of 100000; Copy log copies them all\./);
    section(h, 'scriptLog').tags('button').find(b => b.textContent === 'Copy log').click();
    h.ui.onMessage({kind: 'modsScripts', data: {mods: [{salt: 'doorsx8q', status: 'running', stop: '', lines: 100000}],
                                               salt: 'doorsx8q', total: 100000, full: true, log: all}});
    assert.equal(copied[copied.length - 1].split('\n').length, 100001);
    assert.equal(h.toasts[h.toasts.length - 1].text, 'Copied 100000 lines of the script log.');
  }
  /* the script stops: the row and the details show the stop line, the log its last line */
  h.ui.onMessage({kind: 'modsScripts', data: {mods: [{salt: 'doorsx8q', status: 'stopped', stop: STOP_LINE, lines: 3}], salt: 'doorsx8q',
                                             log: ['door: switch 1 of 3', 'door: switch 2 of 3', STOP_LINE]}});
  assert.equal(iconOf(rowOf(h, DOORS)), 'stop');
  assert.equal(h.nodes.modsDetail.all('mods-detail-sentence')[0].textContent, STOP_LINE);
  assert.equal(section(h, 'scriptLog').all('mods-log-stop')[0].textContent, STOP_LINE);
  /* the next Play: the runtime knows it again as running, the stop is gone */
  h.ui.onMessage({kind: 'modsScripts', data: {mods: [{salt: 'doorsx8q', status: 'running', stop: '', lines: 0}], salt: 'doorsx8q', log: []}});
  assert.equal(iconOf(rowOf(h, DOORS)), 'map', 'back to the in-this-map glyph');
  assert.match(section(h, 'scriptLog').textContent, new RegExp(modsTab.SCRIPT_LOG_EMPTY.replace('.', '\\.')));
  assert.equal(section(h, 'scriptLog').tags('button').find(b => b.textContent === 'Copy log').disabled, true);
  /* Files: the Files view opens on the mod's scripts folder */
  sc.tags('button')[0].click();
  assert.equal(h.ui.state().files.open, true);
  assert.equal(h.ui.state().files.want, 'scripts/');
  /* a mod without scripts has neither section */
  h.ui.closeFiles();
  h.ui.select('alex.r2-imp');
  assert.equal(section(h, 'scripts'), undefined);
  assert.equal(section(h, 'scriptLog'), undefined);
});
test('the script log is asked for each second only while the tab shows a mod with scripts', () => {
  const t = timers();
  const h = harness({store: standIn(v4), every: t.every, cancel: t.cancel});
  assert.equal(t.live.length, 0, 'nothing before the tab shows');
  h.ui.show();
  assert.equal(t.live.length, 1);
  assert.equal(t.live[0].ms, modsTab.SCRIPT_POLL_MS);
  h.sent.length = 0;
  t.tick();
  assert.deepEqual(h.sent, [{cmd: 'modsScripts', salt: ''}], 'no mod with scripts selected: the states only');
  h.ui.select(DOORS);
  h.sent.length = 0;
  t.tick();
  assert.deepEqual(h.sent, [{cmd: 'modsScripts', salt: 'doorsx8q'}]);
  h.ui.hide();
  assert.equal(t.live.length, 0, 'leaving the tab stops it');
  h.ui.show();
  assert.equal(t.live.length, 1);
  /* no mod with scripts: nothing is asked */
  const none = clone(v4);
  none.mods.forEach(b => { delete b.scripts; delete b.scriptRun; });
  h.ui.onMessage({kind: 'mods', data: none});
  assert.equal(t.live.length, 0);
});
test('a mod with scripts says so in its row and its sections; there is no short view and no scripts line of a place', () => {
  const h = live();
  assert.match(rowOf(h, DOORS).textContent, /runs scripts/);
  h.ui.select(DOORS);
  assert.ok(section(h, 'scripts'), 'the Scripts section');
  assert.ok(section(h, 'scriptLog'), 'its log is there for the Play that ran it');
  assert.equal(section(h, 'about').tags('button').length, 1, 'About holds only the folder link');
  assert.ok(!/scriptsShortText|PLACE_KEEP_TEXT|placeText|Came with the map|Scripts run only inside this map/.test(source), 'the places\' words are gone');
  assert.ok(!('scriptsShortText' in modsTab) && !('PLACE_KEEP_TEXT' in modsTab) && !('placeText' in modsTab));
  assert.ok(!h.nodes.modsDetail.textContent.includes('Scripts run only inside this map'));
});
test('a sound bank the game loaded before a change: Restart DOOM to hear the new one', () => {
  const data = clone(v4);
  data.mods.find(b => b.key === DOORS).restartBanks = ['doors_sfx.bnk'];
  const h = live(data);
  h.ui.select(DOORS);
  assert.ok(h.nodes.modsDetail.textContent.indexOf('Restart DOOM to hear the new doors_sfx.bnk.') >= 0);
});
test('the Files view: a script is named as one, its text read with Lua colours', () => {
  const f = modsTab.normFiles({files: [{path: 'scripts/door.lua', kind: 'text', 'class': 'add', size: 9},
                                       {path: 'assets/generated/decls/entitydef/mods/doorsx8q/x.decl', kind: 'text', 'class': 'add', size: 9}]});
  assert.equal(f.files[1].cls, 'script');
  assert.equal(modsTab.fileMeta(f.files[1])[0], modsTab.FILE_CLASSES.script);
  assert.equal(f.files[0].cls, 'add');
  const toks = modsTab.luaTokens('api = 1 -- note\nlocal s = "a\\"b" .. [[long]]\nif x then return nil end --[[ c\n]] 0x1F');
  const kinds = toks.map(t => t.type + (t.isKey ? ':key' : '') + (t.isBool ? ':bool' : ''));
  assert.deepEqual(kinds, ['ident', 'punc', 'num', 'cmt', 'ident:key', 'ident', 'punc', 'str', 'punc', 'punc', 'str', 'ident:key', 'ident',
                           'ident:key', 'ident:key', 'ident:bool', 'ident:key', 'cmt', 'num']);
  const text = 'api = 1 -- note\nlocal s = "a\\"b" .. [[long]]\nif x then return nil end --[[ c\n]] 0x1F';
  assert.equal(text.slice(toks[3].start, toks[3].end), '-- note');
  assert.equal(text.slice(toks[17].start, toks[17].end), '--[[ c\n]]');
  /* the view uses them for a .lua, the decl tokens for everything else */
  let declAsked = 0;
  const folders = {};
  folders[DOORS] = {files: [{path: 'scripts/door.lua', size: 20, kind: 'text', 'class': 'add', game: false,
                             container: '', inFolder: true, cause: ''}],
                    texts: {'scripts/door.lua': 'api = 1\n-- a comment\n'}};
  const h = harness({store: standIn(v4, folders), tokens: t => { declAsked++; return []; }});
  h.ui.show();
  h.ui.openFiles(DOORS, 'scripts/');
  assert.equal(h.ui.state().files.selected, 'scripts/door.lua');
  assert.equal(declAsked, 0, 'a script is not read as a decl');
  const code = h.nodes.modsFilesHost.all('tk-cmt');
  assert.equal(code.length, 1);
  assert.equal(code[0].textContent, '-- a comment');
  assert.ok(h.nodes.modsFilesHost.tags('button').some(b => b.textContent === 'Open in editor'), 'Open in editor on a .lua');
});
test('the Entities tab: a Script node offers its mod\'s names and says when no script uses its name', () => {
  const mods = modsTab.normalize(v4).mods;
  let i = modsTab.scriptEntityInfo(mods, 'mods/doorsx8q/script', 'switch_used');
  assert.equal(i.kind, 'node');
  assert.equal(i.text, 'Script node: runs "switch_used" in Doors.');
  assert.deepEqual(i.choices.map(c => c.name), ['switch_used', 'open_door']);
  i = modsTab.scriptEntityInfo(mods, 'mods/doorsx8q/script', 'open_door');
  assert.equal(i.text, 'Script node: the scripts of Doors fire it as "open_door"; wire its On Fired.');
  i = modsTab.scriptEntityInfo(mods, 'mods/doorsx8q/script', 'opne_door');
  assert.equal(i.used, false);
  assert.equal(i.text, 'No script in Doors uses the name "opne_door"; pick a name its scripts use.');
  i = modsTab.scriptEntityInfo(mods, 'mods/doorsx8q/script', '');
  assert.equal(i.text, 'Script node of Doors: give it the name of something its scripts handle or fire.');
  i = modsTab.scriptEntityInfo(mods, 'mods/DOORSX8Q/props/door_switch', 'Switch 1');
  assert.deepEqual([i.kind, i.text], ['driven', 'Driven by a script in Doors.']);
  assert.equal(modsTab.scriptEntityInfo(mods, 'mods/doorsx8q/props/other', ''), null, 'an entity no script binds');
  assert.equal(modsTab.scriptEntityInfo(mods, 'ai/demon/imp_coop', ''), null, 'a game entity');
  assert.equal(modsTab.scriptEntityInfo(mods, 'mods/alexrimp/script', 'x'), null, 'a mod without scripts');
  /* a name only set while running is not listed: the node says so */
  i = modsTab.scriptEntityInfo([{salt: 'dyn12345', key: 'dyn', name: 'Dyn', folder: 'dyn',
                                 scripts: {files: 1, entities: [], inputs: ['a'], outputs: [], dynamic: 2}}], 'mods/dyn12345/script', 'b');
  assert.match(i.text, /Names its scripts set while running are not listed\.$/);
  /* through the tab: the last document; asked for once when none came */
  const h = harness();
  assert.equal(h.ui.scriptInfo('mods/doorsx8q/script', 'x'), null);
  assert.equal(h.ui.ensureLoaded(), true);
  assert.deepEqual(h.sent[h.sent.length - 1], {cmd: 'modsList'});
  assert.equal(h.ui.ensureLoaded(), false, 'asked once');
  h.ui.onMessage({kind: 'mods', data: v4});
  assert.equal(h.ui.scriptInfo('mods/doorsx8q/script', 'switch_used').text, 'Script node: runs "switch_used" in Doors.');
});
test('the setting: Play Maps Whose Mods Include Scripts is a checkbox item of the Settings menu, with its one sentence', () => {
  const saved = [];
  const h = harness({saveSetting: (k, v) => saved.push([k, v])});
  const item = h.nodes.menuScriptedMaps;
  assert.equal(modsTab.SCRIPTED_MAPS_LABEL, 'Play Maps Whose Mods Include Scripts');
  assert.equal(modsTab.SCRIPTED_MAPS_KEY, 'mods.scripted_maps');
  assert.equal(h.nodes.menuScriptedMapsLabel.textContent, 'Play Maps Whose Mods Include Scripts');
  assert.equal(h.nodes.menuScriptedMapsNote.textContent, modsTab.SCRIPTED_MAPS_NOTE);
  assert.equal(item.getAttribute('role'), 'menuitemcheckbox');
  assert.equal(item.getAttribute('aria-checked'), 'true', 'on by default');
  assert.equal(item.title, modsTab.SCRIPTED_MAPS_NOTE, 'its hover says what it does');
  /* a click toggles it and saves the new value */
  item.click();
  assert.equal(item.getAttribute('aria-checked'), 'false');
  assert.deepEqual(saved, [['mods.scripted_maps', false]]);
  item.click();
  assert.equal(item.getAttribute('aria-checked'), 'true');
  assert.deepEqual(saved, [['mods.scripted_maps', false], ['mods.scripted_maps', true]]);
  /* the saved setting, read by the page at start, sets the box without saving it again */
  h.ui.setScriptedMaps(false);
  assert.equal(item.getAttribute('aria-checked'), 'false');
  h.ui.setScriptedMaps(true);
  assert.equal(item.getAttribute('aria-checked'), 'true');
  h.ui.setScriptedMaps(false);
  item.click();
  assert.equal(item.getAttribute('aria-checked'), 'true', 'the click follows the value the page set');
  assert.equal(saved.length, 3);
  assert.deepEqual(saved[2], ['mods.scripted_maps', true]);
  /* a setting that was never saved is on */
  h.ui.setScriptedMaps(undefined);
  assert.equal(item.getAttribute('aria-checked'), 'true');
  /* the tab's own page no longer carries the setting */
  assert.ok(!/modsScripted/.test(source), 'no modsScriptedMaps id in the tab\'s code');
});

test('the map modal: a mod whose map row is another version than the installed one offers Update', () => {
  const doc = clone(v4);
  const sentence = 'This map uses another version of R2 Imp copy than the one you have.';
  doc.map.mods[0].update = sentence;
  const h = live(doc);
  h.nodes.modsMapBtn.click();
  const mr = h.nodes.modsMapHost.all('mods-map-row');
  assert.ok(mr.length >= 2);
  /* the row with `update` shows the sentence and the button; the others show neither */
  assert.ok(mr[0].textContent.indexOf(sentence) >= 0, 'the sentence');
  const buttons = h.nodes.modsMapHost.tags('button').filter(b => b.textContent === 'Update');
  assert.equal(buttons.length, 1, 'one Update button, on the row that has update');
  assert.ok(mr[1].textContent.indexOf('another version') < 0 && mr[1].textContent.indexOf('Update') < 0);
  const before = h.sent.length;
  buttons[0].click();
  const sent = h.sent.slice(before).filter(m => m.cmd === 'modsAction');
  assert.equal(sent.length >= 1, true);
  assert.deepEqual(sent[0], {cmd: 'modsAction', action: 'mapUpdate', arg: doc.map.mods[0].salt});
  /* a map with no row that has update shows no Update button */
  const plain = live(clone(v4));
  plain.nodes.modsMapBtn.click();
  assert.equal(plain.nodes.modsMapHost.tags('button').filter(b => b.textContent === 'Update').length, 0);
  assert.ok(modalText(plain).indexOf('another version') < 0);
});
test('page contract', () => {
  assert.ok(html.includes('<button type="button" class="tab" role="tab" id="tab-mods" data-tab="mods" aria-controls="panel-mods" aria-selected="false" tabindex="-1">Mods</button>'));
  IDS.forEach(id => assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id));
  GONE.forEach(id => assert.ok(!html.includes('id="' + id + '"'), 'mockup.html no longer has #' + id));
  const panel = html.slice(html.indexOf('<div class="content" id="panel-mods"'), html.indexOf('<div class="statusbar">'));
  const head = panel.slice(panel.indexOf('<div class="panel-head">'), panel.indexOf('<div class="panel-toolbar">'));
  const actions = head.slice(head.indexOf('<div class="panel-actions">'));
  for (const id of ['modsAttnBtn', 'modsMapBtn', 'modsRefreshBtn', 'modsNewBtn', 'modsInstallBtn'])
    assert.match(actions, new RegExp('<button type="button" class="btn (primary )?icon icon-button( mods-attn)?" id="' + id + '"'), id + ' is a panel icon button');
  assert.deepEqual((actions.match(/id="(\w+)"/g) || []).map(s => s.slice(4, -1)), ['modsAttnBtn', 'modsMapBtn', 'modsRefreshBtn', 'modsInstallBtn', 'modsNewBtn'],
                   'the header\'s buttons, in order: New mod last, the folder button gone');
  for (const [id, ic] of [['modsNewBtn', 'plus'], ['modsInstallBtn', 'download'], ['modsMapBtn', 'map'], ['modsRefreshBtn', 'refresh-cw']])
    assert.match(panel, new RegExp('id="' + id + '"[^>]*>(<svg class="ui-icon"[^>]*><use href="#icon-' + ic + '">)'), id + ' is the ' + ic + ' icon');
  assert.match(actions, /class="btn primary icon icon-button" id="modsNewBtn"/, 'New mod is the primary button');
  assert.ok(!/class="btn primary[^"]*" id="modsInstallBtn"/.test(actions), 'Install is a plain icon button');
  assert.ok(!/id="modsInstallBtn"[^>]*aria-haspopup="menu"/.test(actions), 'Install opens no menu');
  /* the mods folder: the foot's path is a button */
  assert.match(panel, /<div class="mods-root" id="modsRoot" role="button" tabindex="0" aria-label="Open the mods folder in Explorer"><\/div>/);
  /* the scripts setting is an item of the Settings menu, checked by default, with its one sentence */
  assert.match(html, /<button type="button" id="menuScriptedMaps" role="menuitemcheckbox" aria-checked="true" aria-describedby="menuScriptedMapsNote"><span id="menuScriptedMapsLabel">Play Maps Whose Mods Include Scripts<\/span><\/button>/);
  assert.match(html, /<div class="menu-note" id="menuScriptedMapsNote"><\/div>/);
  assert.ok(!/modsScripted/.test(html + standin), 'the old checkbox is gone from the page');
  assert.ok(html.includes('after: function(fn, ms){ return window.setTimeout(fn, ms); }'), 'the page gives the tab its one-shot timer');
  assert.ok(html.includes('modsUi.mapChanged()'), 'a change of what the map places asks the tab for a fresh document');
  assert.match(panel, /id="modsRefreshBtn" title="Refresh: after you change a mod's files, build them again and bring the change into the game"/);
  const detailHead = panel.slice(panel.indexOf('<span class="panel-title">Mod Details</span>'), panel.indexOf('id="modsDetail"'));
  assert.deepEqual((detailHead.match(/<button [^>]*id="(\w+)"/g) || []).map(t => t.match(/id="(\w+)"/)[1]),
                   ['modsFilesBtn', 'modsUninstallBtn'], 'the details head: Files and Uninstall; the map action is the In this map block\'s');
  assert.ok(!/migrat|verify/i.test(panel), 'no Migrate and no Verify in the tab');
  assert.match(html, /id="modsList" role="listbox" tabindex="0"/);
  assert.equal(html.split('<script src="mods_tab.js"></script>').length, 2, 'one module tag');
  assert.match(html, /if \(which === 'mods'\) \{ modsUi\.show\(\); post\(\{cmd:'budgetUsage'\}\); \}/, 'showing the tab rescans and reads the map\'s limits');
  /* the browser preview's data is the v4 fixture; the release page carries no stand-in */
  const m = standin.match(/\nvar PREVIEW_MODS = (\{.*\});\r?\n/);
  assert.ok(m, 'PREVIEW_MODS is one JSON line');
  assert.deepEqual(JSON.parse(m[1]), v4, 'PREVIEW_MODS is the v4 fixture');
  assert.ok(!/standIn|stand-in/.test(source), 'mods_tab.js, which the release page carries, holds no stand-in host');
  const pf = standin.match(/\nvar PREVIEW_MOD_FILES = (\{.*\});\r?\n/);
  assert.ok(pf, 'PREVIEW_MOD_FILES is one JSON line');
  assert.ok(JSON.parse(pf[1])['alex.r2-imp'].files.every(f => f.path && f.kind && 'class' in f), 'the preview lists files as modFiles does');
  /* The one repeating request (the script log) goes through the page's timer, never the browser's. */
  assert.ok(!/(^|[^.\w])(setInterval|setTimeout|requestAnimationFrame)\s*\(/.test(source), 'the tab owns no timer of its own');
  assert.ok(html.includes('every: function(fn, ms){ return window.setInterval(fn, ms); }'), 'the page gives the tab its timer');
  assert.ok(html.includes('else modsUi.hide();'), 'leaving the tab stops the script log requests');
  for (const id of ['menuScriptedMaps', 'menuScriptedMapsLabel', 'menuScriptedMapsNote', 'fScriptNote', 'fScriptNames'])
    assert.ok(html.includes('id="' + id + '"'), 'mockup.html has #' + id);
  assert.ok(html.includes('list="fScriptNames"'), 'the Displayname offers the Script node names');
  assert.ok(html.includes('configGet(modsTab.SCRIPTED_MAPS_KEY);'), 'the page reads the scripts setting');
  assert.ok(!/innerHTML/.test(source), 'all text goes through textContent');
  assert.ok(!/[^\x00-\x7f]/.test(source), 'ASCII only');
  assert.ok(!/migrat/i.test(source), 'the Migrate code is gone');
  /* the payload spec */
  /* the preview's content answer carries rows parallel to its lines, "" for the storage line */
  assert.match(standin, /if \(a === 'content'\)[\s\S]*?rows: \['', '[^']+'/, 'the preview answers content with rows');
  for (const k of ['"salt"', '"borrows"', '"sameFilesAs"', '"cause"', '"editorRowNames"', '"facts"',
                   '"requirements"', '"publishedMaps"', '"warnings"', 'mapRemoveModPreview', 'uninstallPreview', 'mapAddMod',
                   '"placedEntities"', '"references"', '"vanilla"', '"savedMaps"', '"undoCleared"', '"attached"', 'addToMine', 'openFile',
                   '"scripts"', '"scriptRun"', '"restartBanks"', 'modsScripts', 'mods.scripted_maps', '"asked"'])
    assert.ok(docs.indexOf(k) >= 0, 'docs/webview-ui.md documents ' + k);
});
/* ---- Maps on this computer: the list's last group, from the content answer ------------------------- */
const KEPT = {storage: 'Map content: 1.3 GB for 14 maps.',
              lines: ['Eternal Demons: Hell Guards, 3 objects, 205.8 MB (40.1 MB of it shared with other maps).',
                      'Eternal Demons: Door Pack, 2 objects, 12.0 MB.'],
              rows: ['maps\\Eternal Demons\\Hell Guards', 'maps\\Eternal Demons\\Door Pack']};
test('the content answer: lines and a parallel rows array of tokens; "" marks the storage line', () => {
  assert.deepEqual(modsTab.normContent(null), {storage: '', rows: [], known: true});
  assert.deepEqual(modsTab.normContent({storage: 'S', lines: ['A', 'B'], rows: ['ta', 'tb']}),
                   {storage: 'S', rows: [{text: 'A', token: 'ta'}, {text: 'B', token: 'tb'}], known: true});
  /* the storage line as the "" row of lines, when no storage member says it */
  assert.deepEqual(modsTab.normContent({lines: ['Map content: 1 GB for 2 maps.', 'A', 'B'], rows: ['', 'ta', 'tb']}),
                   {storage: 'Map content: 1 GB for 2 maps.', rows: [{text: 'A', token: 'ta'}, {text: 'B', token: 'tb'}], known: true});
  /* a storage member wins; a line with no token is not a row; empty lines are skipped */
  const n = modsTab.normContent({storage: 'S', lines: ['Map content: x', '', 'A', 'C'], rows: ['', 'tz', 'ta']});
  assert.deepEqual(n, {storage: 'S', rows: [{text: 'A', token: 'ta'}], known: true});
  assert.deepEqual(modsTab.normContent({lines: ['only a line']}), {storage: 'only a line', rows: [], known: true}, 'an answer without rows has no rows');
  assert.deepEqual(modsTab.normContent({storage: 7, lines: 'no', rows: {}}), {storage: '7', rows: [], known: true});
});
test('Maps on this computer: the last group of the list, one row per kept map\'s mod, shown only when there is something to show', () => {
  const h = live(v4, KEPT);
  const areas = h.nodes.modsList.children;
  assert.equal(areas.length, 2);
  assert.equal(areas[0].getAttribute('data-group'), 'area:own');
  const g = areas[1];
  assert.equal(g.getAttribute('data-group'), 'area:content');
  assert.match(g.className, /\bmods-content\b/);
  assert.equal(g.all('pf-fname')[0].textContent, 'Maps on this computer');
  assert.equal(g.all('pf-fcount')[0].textContent, '2');
  assert.equal(g.all('pf-folder-head')[0].title, KEPT.storage);
  assert.equal(g.all('mods-content-storage')[0].textContent, KEPT.storage);
  const lines = g.all('mods-content-line');
  assert.deepEqual(lines.map(n => n.all('mods-content-text')[0].textContent), KEPT.lines);
  lines.forEach(n => {
    assert.equal(n.getAttribute('role'), 'listitem');
    assert.equal(iconOf(n), 'map', 'the map glyph');
    assert.equal(n.tags('button').length, 1);
    assert.equal(n.tags('button')[0].textContent, 'Add to my mods');
    assert.equal(n.tags('button')[0].title, modsTab.ADD_MINE_TITLE);
    assert.equal(n.tags('button')[0].disabled, false);
  });
  assert.ok(!/\b(store|hash|salt)\b|[A-Z]:[\\/]/i.test(g.textContent), 'no store, hash, salt or path in the words');
  assert.ok(!KEPT.rows.some(t => g.textContent.indexOf(t) >= 0), 'the token is never read');
  /* the body ends with the one removal */
  const last = g.all('pf-folder-body')[0].children.slice(-1)[0];
  assert.equal(last.textContent, 'Remove content of maps you only played');
  assert.equal(last.tagName, 'button');
  assert.equal(last.title, 'Maps you kept are not touched; playing one of the others again gets its content back');
  /* the group is a mod list neighbour, not a mod: the rows and the arrow keys are the mods' alone */
  assert.deepEqual(keys(h), ['campaign-interactables\\doors', 'Arena Gates', 'Arena Gates (old)', 'bare-hands', 'Broken Barrels', 'Editor Grid', 'alex.r2-imp']);
  /* it closes with its head like any group */
  g.all('pf-folder-head')[0].click();
  assert.equal(h.nodes.modsList.children[1].all('mods-content-line').length, 0);
  assert.equal(h.nodes.modsList.children[1].all('pf-folder-head')[0].getAttribute('aria-expanded'), 'false');
  h.nodes.modsList.children[1].all('pf-folder-head')[0].click();
  assert.equal(h.nodes.modsList.children[1].all('mods-content-line').length, 2);
  /* a filter or the attention filter hides it: they are for mods */
  h.nodes.modsFilter.value = 'arena';
  h.nodes.modsFilter.dispatch('input');
  assert.equal(h.nodes.modsList.children.length, 1, 'a filter hides the group');
  h.nodes.modsFilter.value = '';
  h.nodes.modsFilter.dispatch('input');
  assert.equal(h.nodes.modsList.children.length, 2);
  h.nodes.modsAttnBtn.click();
  assert.equal(h.nodes.modsList.children.length, 1, 'the attention filter hides the group');
  h.nodes.modsAttnBtn.click();
  assert.equal(h.nodes.modsList.children.length, 2);
  /* the storage line alone is enough; nothing at all is no group; an empty list of mods keeps its empty card alone */
  const only = live(v4, {storage: 'Map content: 0 B for 0 maps.', lines: [], rows: []});
  assert.equal(only.nodes.modsList.children.length, 2);
  assert.equal(only.nodes.modsList.children[1].all('pf-fcount')[0].textContent, '0');
  assert.equal(only.nodes.modsList.children[1].all('mods-content-line').length, 0);
  assert.equal(live(v4, {storage: '', lines: [], rows: []}).nodes.modsList.children.length, 1, 'no content, no group');
  assert.equal(live(v4).nodes.modsList.children.length, 1, 'an empty answer from the host: no group');
  const none = live(Object.assign(clone(v4), {mods: []}), KEPT);
  assert.equal(none.nodes.modsList.all('mods-empty').length, 1);
  assert.equal(none.nodes.modsList.all('mods-content').length, 0, 'no mods: the empty card alone');
  /* the group follows the content answer that arrives after the list, and shows after the first answer only */
  const late = harness();
  late.ui.onMessage({kind: 'mods', data: v4});
  assert.equal(late.nodes.modsList.children.length, 1, 'nothing before the first answer');
  late.ui.onMessage({kind: 'modsActionResult', action: 'content', ok: true, data: {storage: 'S', lines: ['A'], rows: ['t']}});
  assert.equal(late.nodes.modsList.children.length, 2);
  late.ui.onMessage({kind: 'modsActionResult', action: 'content', ok: false, message: 'No.', data: {}});
  assert.equal(late.nodes.modsList.children.length, 2, 'a failed read leaves what was known');
});
test('Maps on this computer: Add to my mods posts the token; the copy is selected in your mods', () => {
  const h = live(v4, KEPT);
  const add = h.nodes.modsList.children[1].all('mods-content-line')[0].tags('button')[0];
  add.click();
  assert.deepEqual(h.sent.filter(m => m.action === 'addToMine'), [{cmd: 'modsAction', action: 'addToMine', arg: 'maps\\Eternal Demons\\Hell Guards'}]);
  assert.equal(h.toasts.pop().text, 'Added Hell Guards to your mods, in your folder Hell Guards; your own maps can use it now.');
  assert.equal(h.ui.state().selectedKey, 'Hell Guards', 'your new copy is selected');
  assert.ok(rowOf(h, 'Hell Guards'), 'it is one of your mods now');
  assert.equal(h.nodes.modsBadge.textContent, '8');
  assert.equal(inMap(h).state, 'out');
  assert.equal(h.nodes.modsList.children.length, 2, 'the group stays');
  const second = h.nodes.modsList.children[1].all('mods-content-line')[1].tags('button')[0];
  second.click();
  assert.deepEqual(h.sent.filter(m => m.action === 'addToMine').slice(-1)[0], {cmd: 'modsAction', action: 'addToMine', arg: 'maps\\Eternal Demons\\Door Pack'});
  assert.ok(rowOf(h, 'Door Pack'));
  /* a refusal: the status line says it, nothing is selected */
  h.ui.act('addToMine', 'maps\\Gone\\Gone');
  assert.deepEqual(h.toasts.pop(), {text: 'That map is no longer on this computer.', kind: 'err'});
  assert.equal(h.nodes.modsStatus.className, 'mods-status err');
  /* held while another action runs */
  const b = harness();
  shown(b);
  b.ui.onMessage({kind: 'modsActionResult', action: 'content', ok: true, data: {storage: 'S', lines: ['A'], rows: ['t']}});
  const btns = () => b.nodes.modsList.children[1].tags('button');
  assert.deepEqual(btns().map(x => x.disabled), [false, false]);
  b.nodes.modsRefreshBtn.click();
  assert.deepEqual(btns().map(x => x.disabled), [true, true], 'Add to my mods and the removal are held while busy');
  const before = b.sent.length;
  btns()[0].click();
  assert.equal(b.sent.length, before, 'a held button posts nothing');
});
test('Maps on this computer: the removal of what was only played reads the answer again; the group follows the result', () => {
  const h = live(v4, KEPT);
  const box = () => h.nodes.modsList.children[1];
  assert.deepEqual(h.sent.filter(m => m.action === 'content'), [{cmd: 'modsAction', action: 'content'}], 'read once the fresh list arrived');
  box().tags('button').slice(-1)[0].click();
  assert.deepEqual(h.sent.filter(m => m.action === 'content'),
                   [{cmd: 'modsAction', action: 'content'}, {cmd: 'modsAction', action: 'content', arg: 'removePlayed'}]);
  assert.equal(h.nodes.modsStatus.textContent, 'Removed 1.1 GB of content of maps you only played; playing one of them again gets it back.');
  assert.equal(h.nodes.modsStatus.className, 'mods-status ok');
  assert.equal(h.toasts.length, 1);
  assert.equal(h.ui.state().busy, '');
  assert.equal(box().all('mods-content-storage')[0].textContent, 'Map content: 205.8 MB for 1 map.', 'the section follows the result');
  assert.equal(box().all('mods-content-line').length, 0);
  assert.equal(box().all('pf-fcount')[0].textContent, '0');
  /* a plain read says nothing; Refresh reads it again */
  h.nodes.modsRefreshBtn.click();
  assert.equal(h.sent.filter(m => m.action === 'content').length, 3);
  assert.equal(h.toasts.length, 1, 'a plain read of the content says nothing');
});
/* ---- the open map changed: a fresh document, at most once per 400 ms, while the tab or the window shows - */
function clock() {
  const c = {timers: [], after: (fn, ms) => { const t = {fn, ms, fired: false}; c.timers.push(t); return t; },
             fire: () => { const t = c.timers.filter(x => !x.fired); t.forEach(x => { x.fired = true; x.fn(); }); return t.length; },
             pending: () => c.timers.filter(x => !x.fired).length};
  return c;
}
const asks = h => h.sent.filter(m => m.cmd === 'modsList');
test('mapChanged: asks for a fresh document through the page\'s one-shot timer, at most once per 400 ms', () => {
  const c = clock();
  const h = harness({after: c.after});
  /* not loaded: nothing to refresh yet */
  assert.equal(h.ui.mapChanged(), false);
  h.ui.show();
  assert.equal(h.ui.mapChanged(), false, 'a document is already on its way (the rescan)');
  answer(h, v4);
  assert.equal(c.timers.length, 0);
  assert.equal(h.ui.mapChanged(), true);
  assert.equal(c.timers.length, 1);
  assert.equal(c.timers[0].ms, 400);
  assert.equal(asks(h).length, 0, 'nothing is asked until the timer fires');
  /* a burst of changes: one request */
  for (let i = 0; i < 5; i++) assert.equal(h.ui.mapChanged(), false, 'one is already pending');
  assert.equal(c.timers.length, 1);
  assert.equal(c.fire(), 1);
  assert.deepEqual(asks(h), [{cmd: 'modsList'}]);
  assert.equal(h.ui.state().waiting, true);
  assert.equal(h.ui.state().busy, '', 'a quiet read never holds the page');
  assert.equal(h.ui.mapChanged(), false, 'not while the answer is awaited');
  assert.equal(c.timers.length, 1);
  /* the answer comes; the next change schedules again */
  h.ui.onMessage({kind: 'mods', data: v4});
  assert.equal(h.ui.state().waiting, false);
  assert.equal(h.ui.mapChanged(), true);
  assert.equal(c.timers.length, 2);
  assert.equal(c.fire(), 1);
  assert.equal(asks(h).length, 2);
  /* the document that follows moves the glyphs: an object placed in the editor */
  const placed = clone(v4);
  placed.map.mods.push(mapRow({key: 'Arena Gates', salt: 'gatesk2m', name: 'Arena Gates', objects: ['Gate'], placedCount: 1, viaNeeds: false}));
  assert.equal(iconOf(rowOf(h, 'Arena Gates')), 'ok');
  h.ui.onMessage({kind: 'mods', data: placed});
  assert.equal(iconOf(rowOf(h, 'Arena Gates')), 'map');
  /* while another action runs, the timer fires and asks nothing: that action ends with a fresh document */
  assert.equal(h.ui.mapChanged(), true);
  h.ui.act('uninstallPreview', 'Arena Gates');
  assert.equal(h.ui.state().busy, 'uninstallPreview');
  const before = asks(h).length;
  assert.equal(c.fire(), 1);
  assert.equal(asks(h).length, before);
  assert.equal(h.ui.state().waiting, false);
});
test('mapChanged: only while the tab is shown or the This map window is open', () => {
  const c = clock();
  const h = harness({after: c.after});
  h.ui.onMessage({kind: 'mods', data: v4});   /* a document, but the tab was never shown */
  assert.equal(h.ui.state().loaded, true);
  assert.equal(h.ui.mapChanged(), false, 'the tab is not shown');
  assert.equal(c.timers.length, 0);
  assert.deepEqual(asks(h), []);
  /* the window open over a hidden tab */
  h.nodes.modsMapBtn.click();
  assert.equal(h.ui.mapChanged(), true);
  assert.equal(c.fire(), 1);
  assert.equal(asks(h).length, 1);
  h.ui.onMessage({kind: 'mods', data: v4});
  h.ui.closeMap();
  assert.equal(h.ui.mapChanged(), false, 'the window closed again');
  /* the tab shown, then hidden */
  answer(h, v4);
  h.ui.show();
  answer(h, v4);
  assert.equal(h.ui.mapChanged(), true);
  c.fire();
  h.ui.onMessage({kind: 'mods', data: v4});
  h.ui.hide();
  assert.equal(h.ui.mapChanged(), false, 'the tab is hidden');
  assert.equal(c.pending(), 0);
  /* a fresh document with the window open follows the map: the window's rows are the document's */
  const w = harness({after: c.after});
  w.ui.onMessage({kind: 'mods', data: v4});
  w.nodes.modsMapBtn.click();
  assert.equal(w.nodes.modsMapHost.all('mods-map-row').length, 4);
  const fewer = clone(v4);
  fewer.map.mods = fewer.map.mods.slice(0, 2);
  w.ui.onMessage({kind: 'mods', data: fewer});
  assert.equal(w.nodes.modsMapHost.all('mods-map-row').length, 2, 'the window follows the document');
});
test('mapChanged: a page without a timer asks at once, and never while an answer is awaited', () => {
  const h = harness();
  h.ui.show();
  answer(h, v4);
  assert.equal(h.ui.mapChanged(), true);
  assert.deepEqual(asks(h), [{cmd: 'modsList'}]);
  assert.equal(h.ui.mapChanged(), false, 'waiting for the answer');
  assert.equal(asks(h).length, 1);
});
test('host and interface contract', () => {
  assert.ok(host.includes('} else if (cmd == L"modsAction") {'));
  assert.ok(host.includes('SH_IFACE_HAS_SLOT(g_iface, mods)'), 'an older backend is never called past its table');
  for (const a of modsTab.ACTIONS.filter((x) => x !== 'openFolder' && x !== 'openFile'))
    assert.ok(hostMessage.includes('return L"' + a + '";'), 'every page action reaches the backend: ' + a);
  assert.ok(host.includes('sh_mods_message::parse_action(json, act, arg);') &&
            host.includes('return sh_mods_message::action_code(name);'), 'the host routes through mods_message.h');
  assert.ok(host.includes('if (act == L"openFolder") ui_mods_open_folder(arg);'), 'openFolder opens the named mod folder');
  assert.ok(host.includes('else if (act == L"openFile") ui_mods_open_file(arg);'), 'openFile opens one file of a mod folder');
  assert.ok(host.includes('ui_mods_relative_file(rel)'), 'openFile takes a path below the mod folder');
  assert.ok(host.includes('ui_mods_relative_folder(folder)'), 'openFolder takes a folder under the mods folder by its relative path');
  assert.ok(host.includes('ui_mods_post_result(L"unknown", false, L"Unknown mods action.", nullptr);'));
  assert.ok(host.includes('} else if (cmd == L"modsScripts") {'), 'the script log request reaches the host');
  assert.ok(host.includes('call.op = SH_MODS_SCRIPTS;') && host.includes('ui_mods_post(L"{\\"kind\\":\\"modsScripts\\",\\"data\\":", doc, L"}");'),
            'it is answered from the read-only op, as modsScripts');
  assert.ok(/#define SH_MODS_SCRIPTS 7/.test(iface), 'the interface names the op');
});

console.log('mods_tab_test: OK (' + passed + ' tests)');
