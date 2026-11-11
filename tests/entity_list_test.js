/* Entities-list windowing logic lifted from the shipped HTML. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const HTML = path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html');
const src = fs.readFileSync(HTML, 'utf8').replace(/\r\n/g, '\n');
const HOST = path.join(__dirname, '..', 'src', 'ui', 'webview', 'snapmap_plus_ui_webview.cpp');
const hostSrc = fs.readFileSync(HOST, 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}
function sourceNumber(name) {
  const m = src.match(new RegExp('var ' + name + ' = ([0-9]+);'));
  if (!m) throw new Error('could not find ' + name + ' in mockup.html');
  return Number(m[1]);
}

const begin = src.indexOf('  function isFilterUtilityEntity(e)');
const end = src.indexOf('\n  function renderTimelineList()', begin);
if (begin < 0 || end < 0) throw new Error('could not extract Entities list logic');
const logic = src.slice(begin, end);

const nodes = {
  entityList: {scrollTop:0, clientHeight:600, innerHTML:''},
  showHidden: {checked:true},
  filterBox: {value:''},
  entBadge: {textContent:''}
};
const document = {getElementById:function(id){ return nodes[id]; }};
let clock = 0;
const window = {performance:{now:function(){ clock += 0.01; return clock; }}};
function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
function updateSelCount() {}

const api = {};
const prefix =
  'var allEntities = [], visibleEntities = [], visibleEids = [], selectedEids = {};\n' +
  'var entityWindowStart = 0, entityWindowEnd = 0;\n' +
  'var ENTITY_ROW_HEIGHT = ' + sourceNumber('ENTITY_ROW_HEIGHT') + ';\n' +
  'var ENTITY_WINDOW_ROWS = ' + sourceNumber('ENTITY_WINDOW_ROWS') + ';\n' +
  'var ENTITY_WINDOW_STEP = ' + sourceNumber('ENTITY_WINDOW_STEP') + ';\n';
const suffix =
  '\nexports.setEntities = function(v){ allEntities = v; };' +
  '\nexports.setSelected = function(v){ selectedEids = v; };' +
  '\nexports.renderList = renderList;' +
  '\nexports.renderWindow = renderEntityWindow;' +
  '\nexports.bounds = entityWindowBounds;' +
  '\nexports.state = function(){ return {visibleEids:visibleEids.slice(), start:entityWindowStart, end:entityWindowEnd}; };';
new Function('exports', 'window', 'document', 'esc', 'updateSelCount',
  prefix + logic + suffix)(api, window, document, esc, updateSelCount);

const rowHeight = sourceNumber('ENTITY_ROW_HEIGHT');
const rowLimit = sourceNumber('ENTITY_WINDOW_ROWS');
check(rowHeight === 22, 'row pitch changed without updating the CSS/window contract');
check(rowLimit === 128, 'mounted-row safety bound changed unexpectedly');
check(JSON.stringify(api.bounds(8192, 0)) === JSON.stringify({start:0, end:128}),
  'top-of-list window bounds are wrong');
check(JSON.stringify(api.bounds(8192, 8191 * rowHeight)) === JSON.stringify({start:8064, end:8192}),
  'bottom-of-list window bounds are wrong');

const many = Array.from({length:8192}, function(_, i){
  return {eid:i, id:'entity-' + i, name:'Entity ' + i, hidden:false};
});
api.setEntities(many);
nodes.entityList.scrollTop = 0;
let perf = api.renderList();
let state = api.state();
let mounted = (nodes.entityList.innerHTML.match(/class="entity-item/g) || []).length;
check(perf.matched === 8192 && state.visibleEids.length === 8192,
  'large list lost logical entities');
check(perf.mounted === rowLimit && mounted === rowLimit,
  'large list mounted more than the bounded window');
check(nodes.entBadge.textContent === 8192, 'badge no longer reports the full filtered count');

nodes.entityList.scrollTop = 8191 * rowHeight;
api.renderWindow(false);
state = api.state();
mounted = (nodes.entityList.innerHTML.match(/class="entity-item/g) || []).length;
check(state.start === 8064 && state.end === 8192 && mounted === rowLimit,
  'scrolling to the end did not retain a bounded final window');
check(nodes.entityList.innerHTML.indexOf('data-eid="8191"') >= 0,
  'final logical row was not mounted at the bottom');
check(nodes.entityList.innerHTML.indexOf('title="entity-8191: Entity 8191"') >= 0,
  'a clipped entity row no longer exposes its full label on hover');

api.setSelected({8191:true});
api.renderWindow(true);
check(/class="entity-item selected" data-eid="8191"/.test(nodes.entityList.innerHTML),
  'selection styling was lost in a remounted window');

nodes.filterBox.value = 'entity-7000';
nodes.entityList.scrollTop = 0;
perf = api.renderList();
check(perf.matched === 1 && api.state().visibleEids[0] === 7000,
  'filtering no longer uses the complete logical list');

nodes.filterBox.value = '';
nodes.showHidden.checked = false;
api.setEntities([
  {eid:1, id:'normal', name:'Normal', hidden:false},
  {eid:2, id:'hidden', name:'Hidden', hidden:true},
  {eid:3, id:'snapmaps/filter/internal', name:'Utility', hidden:false}
]);
perf = api.renderList();
check(perf.matched === 1 && api.state().visibleEids[0] === 1,
  'hidden/utility filtering changed under windowing');
nodes.showHidden.checked = true;
perf = api.renderList();
check(perf.matched === 2 && api.state().visibleEids.join(',') === '1,2',
  'Show Hidden or utility exclusion changed under windowing');

check(src.indexOf("items[j].addEventListener('mousedown', onRowMouseDown)") < 0,
  'per-row mousedown listener rebinding returned');
check((src.match(/entityListEl\.addEventListener\('mousedown'/g) || []).length === 1,
  'Entities mousedown delegation is missing or duplicated');
check(src.indexOf('ensureEntityIndexVisible(next);') >= 0,
  'keyboard selection no longer mounts its destination window');
check(src.indexOf("post({cmd:'perfList'") >= 0,
  'list-publication performance acknowledgement is missing');
check(hostSrc.indexOf('"perf ui-list: seq=%d') >= 0 &&
      hostSrc.indexOf('"perf list-emit: seq=%lu') >= 0,
  'native/UI list timing correlation is missing');
check(hostSrc.indexOf('perf poll-window:') >= 0 &&
      hostSrc.indexOf('ui_perf_note_poll(') >= 0,
  'complete periodic-task timing is missing');
check(hostSrc.indexOf('std::sort(selids.begin(), selids.end());') >= 0 &&
      hostSrc.indexOf('for (int a = 1; a < sn; a++)') < 0,
  'quadratic selection ordering returned');

if (failures) process.exit(1);
console.log('entity list windowing tests passed');
