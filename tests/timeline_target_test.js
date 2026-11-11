/* A timeline's "Runs on" value names exactly one live entity: its ID or its "ID: name" label,
 * never an entity whose ID merely appears inside the value. Logic lifted from the shipped
 * page and run against a stand-in entity list. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const page = fs.readFileSync(path.join(__dirname, '..', 'src', 'ui', 'webview', 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }

/* One top-level page function, by name, through its closing brace. */
function fn(name) {
  const at = page.indexOf('\n  function ' + name + '(');
  if (at < 0) throw new Error('no function ' + name);
  let i = page.indexOf('{', at), depth = 0;
  for (; i < page.length; i++) {
    if (page[i] === '{') depth++;
    else if (page[i] === '}' && --depth === 0) break;
  }
  return page.slice(at, i + 1);
}

const api = {};
new Function('api', 'allEntities',
  ['tlStripNoModuleSuffix', 'tlFindEntity', 'tlResolveCommitRef', 'tlEntityLabel', 'tlResolveEntityEid'].map(fn).join('\n') +
  '\napi.find = tlFindEntity; api.commit = tlResolveCommitRef; api.label = tlEntityLabel; api.eid = tlResolveEntityEid;')(api, [
  {eid: 7, id: 'snapmaps/ai/ai_7', name: 'Imp'},
  {eid: 74, id: 'snapmaps/ai/ai_74', name: 'Heavy Gunner'},
  {eid: 3, id: 'snapmaps/volume/blocking', name: ''}
]);

/* The reported case: ai_74 must never resolve to ai_7, whose ID is a prefix of it. */
check(api.eid('snapmaps/ai/ai_74') === 74, 'an ID resolves to its own entity');
check(api.eid('snapmaps/ai/ai_74: Heavy Gunner') === 74, 'a label resolves to its own entity');
check(api.label('snapmaps/ai/ai_74') === 'snapmaps/ai/ai_74: Heavy Gunner', 'the label shows the right name');
check(api.commit('snapmaps/ai/ai_74: Heavy Gunner') === 'snapmaps/ai/ai_74', 'a label commits as its ID');
check(api.eid('snapmaps/ai/ai_7') === 7 && api.label('snapmaps/ai/ai_7') === 'snapmaps/ai/ai_7: Imp', 'the shorter ID is its own entity');

/* The (no module) suffix the page adds is ignored; an unnamed entity's label is its ID. */
check(api.eid('snapmaps/ai/ai_74 (no module)') === 74, 'the no-module suffix is ignored');
check(api.label('snapmaps/volume/blocking') === 'snapmaps/volume/blocking', 'an unnamed entity is shown by its ID');

/* A value that only contains a known ID is not that entity, and free text stays as typed. */
check(api.eid('prefix snapmaps/ai/ai_74 suffix') === -1, 'an ID inside other text names nothing');
check(api.eid('snapmaps/ai/ai_74: Someone Else') === -1, 'a label with the wrong name names nothing');
check(api.commit('player1') === 'player1' && api.label('player1') === 'player1', 'free text stays as typed');
check(api.eid('') === -1 && api.label('') === '(no entity)' && api.commit('') === '', 'an empty value names nothing');

if (failures) { console.error(failures + ' timeline target check(s) failed'); process.exit(1); }
console.log('timeline_target_test: all checks passed');
