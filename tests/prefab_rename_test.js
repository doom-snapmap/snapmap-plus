/* The Prefabs tab's Rename on a list row, lifted from the shipped HTML and run against a minimal row
   stand-in: a modded prefab's row also holds its "Mods:" pill (a Blueprint prefab its "Blueprint"
   pill), and the rename must name the file, not the row's visible text, and must keep the pills.
   The answer is checked against the page's stand-in host rule for renamePrefab. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');
const html = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }

function lift(name) {
  const at = html.indexOf('  function ' + name + '(');
  if (at < 0) throw new Error('no function ' + name);
  let i = html.indexOf('{', at), depth = 0;
  for (; i < html.length; i++) {
    if (html[i] === '{') depth++;
    else if (html[i] === '}' && --depth === 0) break;
  }
  return html.slice(at, i + 1);
}

/* A row element: innerHTML is the truth; its first child is the leading text node; textContent is
   every text in it, the pills included, as in a browser. */
function decode(s) { return s.replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&quot;/g, '"').replace(/&#39;/g, "'").replace(/&amp;/g, '&'); }
function makeRow(markup, folder) {
  const attrs = {'data-folder': folder, draggable: 'true'};
  let inner = markup, input = null;
  const row = {
    getAttribute: k => (k in attrs ? attrs[k] : null),
    setAttribute: (k, v) => { attrs[k] = String(v); },
    get innerHTML() { return inner; },
    set innerHTML(v) {
      inner = String(v);
      input = /class="pf-item-name"/.test(inner) ? {value: '', handlers: {}, addEventListener(t, f) { this.handlers[t] = f; }, focus() {}, select() {}} : null;
    },
    get textContent() { return decode(inner.replace(/<[^>]*>/g, '')); },
    set textContent(v) { inner = String(v).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); input = null; },
    get firstChild() {
      const lead = inner.split('<')[0];
      return lead ? {nodeType: 3, nodeValue: decode(lead)} : null;
    },
    querySelector: sel => (sel === '.pf-item-name' ? input : null),
    get input() { return input; }
  };
  return row;
}

const src = lift('pfItemName') + '\n' + lift('renamePrefabItem');
function run(markup, folder, names, typed, key) {
  const posts = [], toasts = [];
  const prefabData = {root: folder ? [] : names.slice(), folders: folder ? [{name: folder, items: names.slice()}] : []};
  const allPrefabNamesInFolder = f => (!f ? prefabData.root.slice() : (prefabData.folders.find(x => x.name === f) || {items: []}).items.slice());
  const api = {};
  new Function('api', 'post', 'toast', 'allPrefabNamesInFolder', src + '\napi.rename = renamePrefabItem;')(
    api, m => posts.push(m), (t, l) => toasts.push({t, l}), allPrefabNamesInFolder);
  const row = makeRow(markup, folder);
  api.rename(row);
  const inp = row.input;
  const shown = inp ? inp.value : null;
  if (inp) { inp.value = typed; inp.handlers.keydown({key: key || 'Enter', preventDefault() {}}); }
  /* The page's stand-in host accepts a rename only when oldName is a prefab in that folder. */
  const accepted = posts.length === 1 && allPrefabNamesInFolder(folder).indexOf(posts[0].oldName) !== -1 &&
                   allPrefabNamesInFolder(folder).indexOf(posts[0].newName) === -1;
  return {posts, toasts, shown, accepted, after: row.innerHTML};
}

const plain = 'Door';
const modded = 'Door<span class="pf-mods" title="test.door 1.0.0 (installed)">Mods: test.door</span>';
const missing = 'Door<span class="pf-mods pf-mods-missing" title="ghost.mod 2.0 (not installed)">Needs mods: ghost.mod</span>';
const blueprint = 'Hall<span class="pf-kind" title="Module prefab: places in Blueprint mode">Blueprint</span>' +
               '<span class="pf-mods" title="test.door 1.0.0 (installed)">Mods: test.door</span>';

let r = run(plain, '', ['Door'], 'Gate');
check(r.shown === 'Door' && r.accepted && r.posts[0].oldName === 'Door' && r.posts[0].newName === 'Gate', 'a plain prefab renames: ' + JSON.stringify(r.posts));

r = run(modded, '', ['Door'], 'Gate');
check(r.shown === 'Door', 'a modded prefab\'s rename field shows its name, not its pill: ' + JSON.stringify(r.shown));
check(r.accepted && r.posts[0].oldName === 'Door' && r.posts[0].newName === 'Gate', 'a modded prefab renames by its file name: ' + JSON.stringify(r.posts));

r = run(missing, 'Doors', ['Door'], 'Gate');
check(r.accepted && r.posts[0].oldName === 'Door' && r.posts[0].folder === 'Doors', 'a prefab missing its mods renames inside its folder: ' + JSON.stringify(r.posts));

r = run(blueprint, '', ['Hall'], 'Atrium');
check(r.shown === 'Hall' && r.accepted && r.posts[0].oldName === 'Hall', 'a Blueprint prefab with mods renames by its file name: ' + JSON.stringify(r.posts));

/* Unchanged, cancelled or colliding: nothing is posted and the row keeps its pills. */
r = run(modded, '', ['Door'], 'Door');
check(r.posts.length === 0 && r.after === modded, 'an unchanged name posts nothing and the row keeps its pill: ' + r.after);
r = run(modded, '', ['Door'], 'Gate', 'Escape');
check(r.posts.length === 0 && r.after === modded, 'Escape posts nothing and the row keeps its pill: ' + r.after);
r = run(modded, '', ['Door', 'Gate'], 'Gate');
check(r.posts.length === 0 && r.toasts.length === 1 && r.after === modded, 'a colliding name warns, posts nothing and keeps the pill');
r = run(modded, '', ['Door'], 'Gate');
check(r.after === modded, 'after the post the row shows its pill until the host refresh redraws it');

if (failures) { console.error('prefab_rename_test: ' + failures + ' failure(s)'); process.exit(1); }
console.log('prefab_rename_test: OK');
