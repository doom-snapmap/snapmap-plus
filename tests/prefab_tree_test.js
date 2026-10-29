/* Prefabs tree logic lifted from the shipped HTML: visible rows, Shift ranges, drop plans. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const HTML = path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html');
const src = fs.readFileSync(HTML, 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}

const begin = src.indexOf('  // --- prefab tree (pure) ---');
const end = src.indexOf('  // --- prefab tree (pure) end ---', begin);
if (begin < 0 || end < 0) throw new Error('could not extract the prefab tree logic');
const keyBegin = src.indexOf('  function pfSelKey(r)');
const keyEnd = src.indexOf('\n', keyBegin);
if (keyBegin < 0) throw new Error('could not extract pfSelKey');
const api = {};
new Function('api', src.slice(keyBegin, keyEnd) + '\n' + src.slice(begin, end) +
  '\napi.pfVisibleRows = pfVisibleRows; api.pfRangeRows = pfRangeRows; api.pfPlanMove = pfPlanMove; api.pfSelKey = pfSelKey;')(api);

const data = {
  root: ['Alpha', 'Beta'],
  folders: [
    {name: 'Doors', items: ['Blast door', 'Hatch']},
    {name: 'Empty', items: []},
    {name: 'Lights', items: ['Alpha', 'Strobe']}
  ]
};
const tags = {'Doors/Hatch': ['small', 'metal'], 'Beta': ['metal']};
const keys = rows => rows.map(r => (r.name ? r.folder + '/' + r.name : '[' + r.folder + ']'));

/* Folders start collapsed: only heads and root items show. */
let rows = api.pfVisibleRows(data, tags, '', {});
check(keys(rows).join('|') === '[Doors]|[Empty]|[Lights]|/Alpha|/Beta', 'collapsed tree lists heads then root: ' + keys(rows).join('|'));
check(rows[0].open === false && rows[0].count === 2, 'a closed folder head carries its count');

/* An open folder lists its items under its head, in order. */
rows = api.pfVisibleRows(data, tags, '', {Lights: true});
check(keys(rows).join('|') === '[Doors]|[Empty]|[Lights]|Lights/Alpha|Lights/Strobe|/Alpha|/Beta', 'open folder rows: ' + keys(rows).join('|'));
check(rows[2].open === true, 'the open folder head is marked open');

/* A filter opens the folders it matches in, drops folders without matches, and matches tags. */
rows = api.pfVisibleRows(data, tags, 'metal', {});
check(keys(rows).join('|') === '[Doors]|Doors/Hatch|/Beta', 'tag filter: ' + keys(rows).join('|'));
check(rows[0].open === true, 'a filtered folder shows open regardless of the saved state');
rows = api.pfVisibleRows(data, tags, 'ALPHA', {});
check(keys(rows).join('|') === '[Lights]|Lights/Alpha|/Alpha', 'name filter is case-insensitive and keeps same-named files apart: ' + keys(rows).join('|'));
rows = api.pfVisibleRows(data, tags, 'zzz', {Doors: true});
check(rows.length === 0, 'no matches gives no rows');

/* Shift ranges run over the visible rows in display order, either direction, folders included. */
const all = api.pfVisibleRows(data, tags, '', {Doors: true, Lights: true});
let range = api.pfRangeRows(all, {folder: 'Doors', name: 'Hatch'}, {folder: 'Lights', name: 'Alpha'});
check(keys(range).join('|') === 'Doors/Hatch|[Empty]|[Lights]|Lights/Alpha', 'forward range: ' + keys(range).join('|'));
range = api.pfRangeRows(all, {folder: 'Lights', name: 'Alpha'}, {folder: 'Doors', name: 'Hatch'});
check(keys(range).join('|') === 'Doors/Hatch|[Empty]|[Lights]|Lights/Alpha', 'backward range gives the same rows');
range = api.pfRangeRows(all, null, {folder: '', name: 'Beta'});
check(keys(range).join('|') === '/Beta', 'no anchor selects the target alone');
range = api.pfRangeRows(all, {folder: 'Doors', name: 'Hatch'}, {folder: 'Nope', name: 'x'});
check(range.length === 0, 'a target that is not visible selects nothing');
/* Collapsed folder items are not in the range. */
const partly = api.pfVisibleRows(data, tags, '', {Lights: true});
range = api.pfRangeRows(partly, {folder: 'Doors', name: ''}, {folder: 'Lights', name: 'Strobe'});
check(keys(range).join('|') === '[Doors]|[Empty]|[Lights]|Lights/Alpha|Lights/Strobe', 'closed folders contribute only their head: ' + keys(range).join('|'));

/* Drop plans: rows already in the target stay, name collisions are skipped, folders stand for their items. */
let plan = api.pfPlanMove([{folder: '', name: 'Beta'}, {folder: 'Doors', name: 'Hatch'}], data, 'Lights');
check(plan.moves.length === 2 && plan.moves[0].name === 'Beta' && plan.moves[0].fromFolder === '' && plan.moves[1].fromFolder === 'Doors', 'two prefabs move into Lights');
check(plan.skipped.length === 0, 'no collisions');
plan = api.pfPlanMove([{folder: '', name: 'Alpha'}, {folder: '', name: 'Beta'}], data, 'Lights');
check(plan.moves.length === 1 && plan.moves[0].name === 'Beta' && plan.skipped.join() === 'Alpha', 'a name already in the target is skipped: ' + JSON.stringify(plan));
plan = api.pfPlanMove([{folder: 'Lights', name: 'Strobe'}], data, 'Lights');
check(plan.moves.length === 0 && plan.skipped.length === 0, 'dropping where it lives is not a move');
plan = api.pfPlanMove([{folder: 'Doors', name: ''}], data, '');
check(plan.moves.length === 2 && plan.moves.every(m => m.fromFolder === 'Doors'), 'a folder dropped on the root moves its prefabs out');
plan = api.pfPlanMove([{folder: 'Lights', name: ''}, {folder: 'Lights', name: 'Strobe'}], data, '');
check(plan.moves.length === 1 && plan.moves[0].name === 'Strobe' && plan.skipped.join() === 'Alpha', 'folder plus its item counts once and skips the root collision: ' + JSON.stringify(plan));
plan = api.pfPlanMove([{folder: 'Doors', name: ''}], data, 'Doors');
check(plan.moves.length === 0, 'a folder dropped on itself is not a move');
plan = api.pfPlanMove([{folder: 'Doors', name: ''}, {folder: '', name: 'Alpha'}], data, 'Empty');
check(plan.moves.length === 3, 'a folder and a root prefab merge into an empty folder');
plan = api.pfPlanMove([{folder: '', name: 'Alpha'}, {folder: 'Lights', name: 'Alpha'}], data, 'Empty');
check(plan.moves.length === 1 && plan.skipped.join() === 'Alpha', 'two same-named prefabs cannot both land in one folder');

/* Settings key, collapsed default, batch commands and styling are wired. */
check(src.indexOf("var PREFAB_OPEN_FOLDERS_KEY = 'prefabs.open_folders';") !== -1, 'the open-folder list has a settings key');
check(src.indexOf('configGet(PREFAB_OPEN_FOLDERS_KEY);') !== -1, 'the open-folder list is requested at startup');
check(src.indexOf("'<div class=\"pf-folder pf-open\"") === -1, 'rendered folders no longer default to open');
check(src.indexOf("post({cmd:'movePrefabs', names: names, fromFolders: froms, toFolder: toFolder})") !== -1, 'a multi-row drop posts one batch move');
check(src.indexOf("d.kind === 'movePrefabsResult'") !== -1 && src.indexOf("d.kind === 'deletePrefabsResult'") !== -1, 'batch results are handled');
const host = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui', 'webview', 'snapmap_plus_ui_webview.cpp'), 'utf8');
check(host.indexOf('cmd == L"movePrefabs"') !== -1 && host.indexOf('cmd == L"deletePrefabs"') !== -1, 'the host accepts the batch commands');
const cfg = fs.readFileSync(path.join(__dirname, '..', 'src', 'backend', 'config.c'), 'utf8');
check(cfg.indexOf('"prefabs.open_folders", SH_JSON_ARRAY, "[]"') !== -1, 'the settings registry carries the open-folder list as an array');
const css = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui', 'webview', 'studio.css'), 'utf8');
check(css.indexOf('.pf-folder-body::before') !== -1 && css.indexOf('.pf-folder-head.selected') !== -1, 'folder guide line and folder selection are styled');

console.log('prefab_tree_test: ' + failures + ' failures');
process.exit(failures ? 1 : 0);
