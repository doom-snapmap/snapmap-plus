/* Timeline eventCall authoring lifted from the shipped HTML: an engine event on a runtime-named entity
 * (player1) with a typed argument commits as the stock idEventCallInfo shape, arguments keyed by type in
 * signature order, and an authored map round-trips through parseTimeline. Also checks that the page's
 * event catalog header carries the event with its live-registry signature. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const src = fs.readFileSync(path.join(ROOT, 'src', 'ui', 'webview', 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const catalog = fs.readFileSync(path.join(ROOT, 'src', 'ui', 'sh_event_catalog.h'), 'utf8');
let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}

/* The catalog entry for a name, as the host's enumEvents message builds it. */
function catalogEntry(name) {
  const key = '{ "' + name + '", ';
  const at = catalog.indexOf(key);
  if (at < 0) return null;
  const row = catalog.slice(at + key.length, catalog.indexOf('}', at)).split(',').map(t => t.trim());
  const args = [];
  if (row[1] !== '0') {
    const head = 'static const ShEvtArg ' + row[1] + '[] = {';
    const h = catalog.indexOf(head);
    const body = catalog.slice(h + head.length, catalog.indexOf('};', h));
    const re = /\{"([^"]*)","([^"]*)"\}/g;
    let x;
    while ((x = re.exec(body)) !== null) args.push({ name: x[1], type: x[2] });
  }
  return { name: name, argc: parseInt(row[0], 10), args: args };
}

const fphb = catalogEntry('forcePraetorHandsAndBody');
check(fphb && fphb.argc === 1 && fphb.args.length === 1 && fphb.args[0].type === 'bool' && fphb.args[0].name === 'force',
      'catalog carries forcePraetorHandsAndBody(force: bool), the live registry signature');
const spawn = catalogEntry('spawnSingleAI');
check(spawn && spawn.args.map(a => a.type).join(',') === 'eEncounterSpawnType_t,entity,string',
      'catalog keeps signature order and the map key spelling (entity, string) for spawnSingleAI');
const count = (catalog.match(/^ {4}\{ "[^"\n]+", \d+, (SH_EVT_A_\d+|0) \},\r?$/mg) || []).length;
check(count === 1611, 'catalog lists the 1611 events of the build 13954591 registry: ' + count);

/* Extract the commit path: argument classification through the patched entity builder, plus the parser. */
const begin = src.indexOf('  // Timeline parsing and rendering.');
const end = src.indexOf('  // Fetch fresh entity JSON for Save.', begin);
if (begin < 0 || end < 0) throw new Error('could not extract the timeline commit path');
const api = {};
new Function('api', 'catalog',
  'var tlModel = null, allEntities = [], eventCatalogByName = catalog, eventDocsByName = {}, argResclassCache = {},' +
  ' argResclassRequested = {}, entityAssetsBySlug = {}, entityInheritCache = {}, entityInheritRequested = {};' +
  ' var prefabTransform = { parseScene: function(t){ return JSON.parse(t); } };' +
  ' function post(){} function esc(s){ return String(s); } function iconSvg(){ return ""; } function isFilterUtilityEntity(){ return false; }' +
  src.slice(begin, end) +
  '\napi.setModel = function(m){ tlModel = m; }; api.setEntities = function(e){ allEntities = e; };' +
  ' api.patch = tlBuildPatchedEntityJson; api.parse = parseTimeline; api.defaultArg = tlDefaultArgValue;')(api,
  { forcePraetorHandsAndBody: fphb, spawnSingleAI: spawn });

/* A timeline as the Timeline tile places it: snapmaps/unknown, class idTarget_Timeline, no component. */
const fresh = {
  customIcon: { targetType: 'idDeclSnapCustomIcon', value: null, '~type': '|pointer' },
  displayName: 'Hands', entityDef: { className: 'idTarget_Timeline', inherit: 'snapmaps/unknown', name: '',
    state: { edit: { spawnPosition: { x: 1, y: 2, z: 3 } } }, targetType: 'idDeclEntityDef', '~type': 'idDeclEntityDef', '~version': 26 },
  uniqueId: 95, '~type': 'idSnapEntity'
};
api.setEntities([{ eid: 95, id: 'm/snapmaps/unknown_95', name: 'Hands' }]);
check(JSON.stringify(api.defaultArg('bool')) === '{"bool":false}', 'a new bool argument seeds as {bool:false}');
/* The model after picking the event, typing player1 in Runs on, and ticking the checkbox. */
api.setModel({ eid: 95, isEncounter: false, active: 0,
  tabs: [{ entity: 'player1', events: [{ time: '0', eventDef: 'forcePraetorHandsAndBody', args: [{ bool: true }] }] }] });
const patched = JSON.parse(api.patch(JSON.stringify(fresh)));
const comp = patched.entityDef.state.edit.componentTimeLine;
check(comp && comp.entityEvents.num === 1, 'one entity-event list');
const tab = comp.entityEvents['item[0]'];
check(tab.entity === 'player1', 'a runtime name with no live editor entity commits unchanged: ' + tab.entity);
const ev = tab.events['item[0]'];
check(tab.events.num === 1 && ev.eventTime === 0, 'event at t=0');
check(ev.eventCall['\neventHandle_t eventDef'] === 'forcePraetorHandsAndBody', 'eventDef written under the engine key');
check(ev.eventCall.args['\nnum'] === 1 && JSON.stringify(ev.eventCall.args['item[0]']) === '{"bool":true}',
      'the argument is keyed by its type: ' + JSON.stringify(ev.eventCall.args));
check(patched.entityDef.className === 'idTarget_Timeline' && patched.entityDef.inherit === 'snapmaps/unknown',
      'the vanilla class and inherit are kept');

/* Several typed arguments keep signature order and the numeric conversion. */
api.setModel({ eid: 95, isEncounter: false, active: 0,
  tabs: [{ entity: 'player1', events: [{ time: '250', eventDef: 'spawnSingleAI',
    args: [{ eEncounterSpawnType_t: 'ENCOUNTER_SPAWN_IMP' }, { entity: 'm/snapmaps/unknown_95' }, { string: 'wave' }] }] }] });
const two = JSON.parse(api.patch(JSON.stringify(fresh))).entityDef.state.edit.componentTimeLine.entityEvents['item[0]'].events['item[0]'];
check(JSON.stringify(two.eventCall.args) === JSON.stringify({ 'item[0]': { eEncounterSpawnType_t: 'ENCOUNTER_SPAWN_IMP' },
  'item[1]': { entity: 'm/snapmaps/unknown_95' }, 'item[2]': { string: 'wave' }, '\nnum': 3 }) && two.eventTime === 250,
  'three arguments keyed by type in signature order: ' + JSON.stringify(two.eventCall.args));

/* Reopening the committed entity reads the same event back. */
const reopened = api.parse(95, JSON.stringify(patched));
check(reopened.ok && reopened.tabs.length === 1 && reopened.tabs[0].entity === 'player1', 'reopen keeps the target');
check(reopened.tabs[0].events[0].eventDef === 'forcePraetorHandsAndBody' && reopened.tabs[0].events[0].time === 0 &&
      JSON.stringify(reopened.tabs[0].events[0].args) === '[{"bool":true}]', 'reopen keeps the event and its argument');

/* --emit <entity.json>: print the patch the tab's Save sends for the forcePraetorHandsAndBody model. */
/* The Runs on note: player targets are flagged, map entities and empty values are not. */
const nb = src.indexOf('  function tlRuntimeTargetNote(ref)');
const ne = src.indexOf('  // Show both ID and display name', nb);
const note = new Function(src.slice(nb, ne) + ' return tlRuntimeTargetNote;')();
check(note('player1').indexOf('never run') >= 0 && note(' Player8 ').indexOf('Player8') >= 0, 'player1..player8 targets carry the note');
check(note('player9') === '' && note('m/snapmaps/unknown_95') === '' && note('') === '' && note(null) === '', 'other targets carry no note');

if (process.argv[2] === '--emit') {
  api.setModel({ eid: 0, isEncounter: false, active: 0,
    tabs: [{ entity: 'player1', events: [{ time: '0', eventDef: 'forcePraetorHandsAndBody', args: [{ bool: true }] }] }] });
  process.stdout.write(api.patch(fs.readFileSync(process.argv[3], 'utf8')));
  process.exit(failures ? 1 : 0);
}
if (failures) { console.error(failures + ' failure(s)'); process.exit(1); }
console.log('timeline_eventcall_test: ok');
