/* Timelines tree logic lifted from the shipped HTML: name paths, folder data, create and move contracts. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const HTML = path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html');
const src = fs.readFileSync(HTML, 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}

/* tlSplitName / tlJoinName / tlTreeData / tlTagMap, with the page globals they read stubbed. */
const begin = src.indexOf('  function tlSplitName(name)');
const end = src.indexOf('  function tlSelKey(r)', begin);
if (begin < 0 || end < 0) throw new Error('could not extract the timelines tree logic');
const pureBegin = src.indexOf('  // --- prefab tree (pure) ---');
const pureEnd = src.indexOf('  // --- prefab tree (pure) end ---', pureBegin);
const keyBegin = src.indexOf('  function pfSelKey(r)');
const keyEnd = src.indexOf('\n', keyBegin);
const renBegin = src.indexOf('  function tlRenameFolderTo(');
const renEnd = src.indexOf('  // Move through visible timeline rows', renBegin);
if (renBegin < 0 || renEnd < 0) throw new Error('could not extract tlRenameFolderTo');
const api = {};
const env = {
  allTimelines: [],
  tlFolders: [],
  tlOpenFolders: {},
  tlLabels: {},
  PREVIEW: true,
  configSet: function(){},
  receiveTimelineOpenFolders: null, renderTimelineList: function(){}
};
new Function('api', 'env',
  'var allTimelines = env.allTimelines, tlFolders = env.tlFolders, tlOpenFolders = env.tlOpenFolders, tlLabels = {}, PREVIEW = true;' +
  'function configSet(){} function renderTimelineList(){} var posted = []; function post(m){ posted.push(m); }' +
  src.slice(keyBegin, keyEnd) + '\n' + src.slice(pureBegin, pureEnd) +
  src.slice(begin, end).replace('var tlFolders = [];', '').replace('var tlOpenFolders = {};', '').replace('var tlSel = [];', 'var tlSel = [];').replace('var tlLabels = {};', '') +
  src.slice(renBegin, renEnd) +
  '\napi.split = tlSplitName; api.join = tlJoinName; api.data = function(){ return tlTreeData(); }; api.labels = function(){ return tlLabels; };' +
  'api.tags = tlTagMap; api.rows = pfVisibleRows; api.plan = pfPlanMove; api.setFolders = function(f){ tlFolders = f; }; api.folders = function(){ return tlFolders; };' +
  'api.renameFolder = tlRenameFolderTo; api.posted = function(){ return posted; };')(api, env);

/* Name paths: the first slash splits folder and leaf; edge cases stay at the root. */
check(JSON.stringify(api.split('Arena/Wave 1')) === JSON.stringify({folder: 'Arena', leaf: 'Wave 1'}), 'folder/leaf split');
check(JSON.stringify(api.split('A/B/C')) === JSON.stringify({folder: 'A', leaf: 'B/C'}), 'only the first slash is the folder');
check(api.split('/x').folder === '' && api.split('x/').folder === '' && api.split('').folder === '', 'leading, trailing or no slash is no folder');
check(api.join('Arena', 'Boss') === 'Arena/Boss' && api.join('', 'Boss') === 'Boss', 'join');

/* Tree data groups timelines by folder, keeps unnamed ones by id, adds empty folders, sorts folders. */
env.allTimelines.push({eid: 7, id: 'm/snapmaps/unknown_7', name: 'Zeta/Last'});
env.allTimelines.push({eid: 3, id: 'm/snapmaps/unknown_3', name: ''});
env.allTimelines.push({eid: 5, id: 'm/snapmaps/unknown_5', name: 'Arena/Wave 1'});
env.allTimelines.push({eid: 9, id: 'm/snapmaps/unknown_9', name: 'Intro'});
api.setFolders(['Empty']);
let data = api.data();
check(data.root.join(',') === '3,9', 'root holds the unnamed and the plain timelines in list order: ' + data.root.join(','));
check(data.folders.map(f => f.name).join(',') === 'Arena,Empty,Zeta', 'folders sorted case-insensitively with the empty one: ' + data.folders.map(f => f.name).join(','));
check(data.folders[0].items.join(',') === '5' && data.folders[1].items.length === 0 && data.folders[2].items.join(',') === '7', 'items by folder');
const labels = api.labels();
check(labels['3'].leaf === 'm/snapmaps/unknown_3' && labels['5'].leaf === 'Wave 1' && labels['5'].folder === 'Arena', 'labels: leaf or id, folder');
const tags = api.tags();
check(tags['Arena/5'] && tags['Arena/5'].indexOf('Wave 1') !== -1 && tags['9'].indexOf('Intro') !== -1, 'the filter sees the leaf, the id and the full name');

/* The prefab helpers serve the tree: collapsed folders, filter opens matches, drop plans rename. */
let rows = api.rows(data, tags, '', {});
check(rows.map(r => r.name ? r.name : '[' + r.folder + ']').join('|') === '[Arena]|[Empty]|[Zeta]|3|9', 'collapsed rows: ' + rows.map(r => r.name || '[' + r.folder + ']').join('|'));
rows = api.rows(data, tags, 'wave', {});
check(rows.length === 2 && rows[0].folder === 'Arena' && rows[0].open === true && rows[1].name === '5', 'a filter opens the matching folder');
let plan = api.plan([{folder: '', name: '9'}, {folder: 'Zeta', name: ''}], data, 'Arena');
check(plan.moves.length === 2 && plan.moves.map(m => m.name).join(',') === '9,7', 'a timeline and a folder move into Arena: ' + JSON.stringify(plan));

/* Folder rename and delete: members are renamed in one batch; an empty folder is re-labelled or dropped. */
api.setFolders(['Empty', 'Zeta']);
api.data();
api.renameFolder('Zeta', 'Omega');
check(api.posted().length === 1 && api.posted()[0].cmd === 'setDisplayNames' && api.posted()[0].names.join() === 'Omega/Last', 'renaming a folder renames its members: ' + JSON.stringify(api.posted()));
check(api.folders().join() === 'Empty', 'a folder with members is not kept in the empty-folder list: ' + api.folders().join());
env.allTimelines[0].name = 'Omega/Last';   // the engine rescan carries the new name back
api.renameFolder('Empty', 'Spare');
check(api.folders().join() === 'Spare', 'an empty folder is re-labelled in place: ' + api.folders().join());
api.renameFolder('Spare', '');
check(api.folders().length === 0 && api.data().folders.map(f => f.name).join() === 'Arena,Omega', 'deleting a folder leaves no empty-named folder behind: ' + JSON.stringify(api.folders()));
api.renameFolder('Omega', '');
env.allTimelines[0].name = 'Last';
check(api.posted()[1].names.join() === 'Last' && api.data().folders.map(f => f.name).join() === 'Arena', 'deleting a folder with members moves them to the root');

/* Contracts with the host and the engine. */
check(src.indexOf("post({cmd:'setDisplayNames', eids: eids, names: names});") !== -1, 'moves and folder renames post one batch rename');
check(src.indexOf("neePrefab({cls: 'idTarget_Timeline', inherit: 'snapmaps/unknown'}") !== -1, 'a new timeline is a one-entity prefab on the portable inherit');
check(src.indexOf("post({cmd:'newEntity', label: 'Timeline', json: tlNewTimelinePrefab(") !== -1, 'creation goes through the New entity staging');
check(src.indexOf("var TIMELINE_OPEN_FOLDERS_KEY = 'timelines.open_folders';") !== -1 && src.indexOf('configGet(TIMELINE_OPEN_FOLDERS_KEY);') !== -1, 'open folders are a setting');
check(src.indexOf('id="tlNewFolderBtn"') !== -1 && src.indexOf('id="createTimelineBtn" disabled title="Create New Timeline: open a map in the editor first"') !== -1, 'the New Folder button exists and Create is only gated by a map');
check(src.indexOf("d.kind === 'setDisplayNamesResult'") !== -1, 'the rename result is handled');
check(src.indexOf('doc = prefabTransform.parseScene(entityJson);') !== -1 && src.indexOf('doc = prefabTransform.parseScene(freshEntityJson);') !== -1, 'the timeline open and save paths strip the build 13954591 block comments before parsing');
check(src.indexOf("t.closest('#prefabTree, #timelineList')") !== -1, 'the document drag guard lets the timelines tree drag');
check(src.indexOf("tlSetFolderOpen(fname, !fel.classList.contains('pf-open'));\n        renderTimelineList();") !== -1, 'a folder click re-renders the list (its rows exist only while open)');
check(src.indexOf("if (tlListSeen) for (var on in tlOpenFolders)") !== -1 && src.indexOf("tlListSeen = true;") !== -1, 'open folders are pruned only once a list has arrived');
const cfg = fs.readFileSync(path.join(__dirname, '..', 'src', 'backend', 'config.c'), 'utf8');
check(cfg.indexOf('"timelines.open_folders", SH_JSON_ARRAY, "[]"') !== -1, 'the settings registry carries the timelines open-folder list');
const host = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui', 'webview', 'snapmap_plus_ui_webview.cpp'), 'utf8');
check(host.indexOf('cmd == L"setDisplayNames"') !== -1, 'the host accepts the batch rename');

console.log('timeline_tree_test: ' + failures + ' failures');
process.exit(failures ? 1 : 0);
