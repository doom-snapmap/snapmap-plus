/* The page's half of mods_inmap_test.c: at every step of tests/fixtures/mods_inmap_cases.json (the document the
   backend wrote, reduced to what the page reads, and the rows the save then wrote), the Mods tab's own rule
   (mods_tab.js mapState) says "In this map" for exactly the installed mods the save writes, the This map window
   lists exactly the save's mods, a mod used by the whole map is said to be one, and a mod of editor tweaks only is
   never in a map. The native half holds the document to the save and the committed cases to its run. Pure ASCII. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const modsTab = require(path.join(__dirname, '..', 'src', 'ui', 'webview', 'mods_tab.js'));
const cases = JSON.parse(fs.readFileSync(path.join(__dirname, 'fixtures', 'mods_inmap_cases.json'), 'utf8'));

const low = s => String(s || '').toLowerCase();
const sorted = list => Array.from(new Set(list.map(low))).sort();
let checked = 0;

assert.ok(Array.isArray(cases) && cases.length >= 15, 'the native half wrote every step');
cases.forEach(c => {
  const model = modsTab.normalize({v: 4, gen: 1, root: '', storeReadable: true, mods: c.mods, map: c.map});
  const save = c.save.map(r => low(r.salt));
  const installed = model.mods.map(b => low(b.salt));
  const states = model.mods.map(b => ({b, s: modsTab.mapState(model.map, b)}));

  /* The badge and the In this map block: the installed mods the save writes, no more and no fewer. */
  const badged = states.filter(x => modsTab.inMapState(x.s)).map(x => x.b.salt);
  assert.deepEqual(sorted(badged), sorted(save.filter(s => installed.indexOf(s) >= 0)),
                   c.name + ': the page says In this map for exactly the save\'s installed mods');

  /* The This map window: every mod the save writes, installed or not. */
  const listed = model.map && model.map.open && model.map.mods ? model.map.mods.map(e => e.salt) : [];
  assert.deepEqual(sorted(listed), sorted(save), c.name + ': the This map window lists exactly the save\'s mods');

  states.forEach(({b, s}) => {
    const row = c.save.filter(r => low(r.salt) === low(b.salt))[0];
    /* A mod of editor tweaks only is never in a map, whatever is placed. */
    if (b.facts && b.facts.editorOnly) assert.equal(s.state, 'editorOnly', c.name + ': ' + b.name + ' is editor tweaks only');
    else if (!model.map || !model.map.open) assert.equal(s.state, 'noMap', c.name + ': no map, no state of a map');
    else if (!row) assert.equal(s.state, 'out', c.name + ': ' + b.name + ' is not in this map');
    else {
      /* Used by the whole map exactly when the save writes its row map-wide; the action is Remove from map. */
      assert.equal(s.state === 'whole', !!row.attached, c.name + ': ' + b.name + ' whole');
      assert.ok(s.head.indexOf(modsTab.MAP_STATE.inPrefix) === 0, c.name + ': ' + b.name + ' says In this map');
      if (s.state === 'whole' || s.state === 'objects') assert.equal(s.action, 'remove', c.name + ': ' + b.name + ' can be removed');
    }
    /* The action that state allows: Use in this map only for a mod with nothing to place, while it is not in it. */
    if (s.action === 'use') assert.ok(!modsTab.placeable(b) && s.state === 'out', c.name + ': ' + b.name + ' use');
    checked++;
  });

  /* What the page says of a pinned row matches the save: the map holds its published copy. */
  (model.map && model.map.mods || []).forEach(e => {
    const row = c.save.filter(r => low(r.salt) === low(e.salt))[0];
    assert.equal(e.pinned, !!(row && row.pinned), c.name + ': ' + e.salt + ' pinned');
  });
});

/* The steps the owner named are in the cases. */
['a mod\'s object placed, before any save', 'the last object of a saved mod deleted, before the next save',
 'that delete undone', 'a mod reached only through another mod\'s object', 'a mod used by the whole map (Use in this map)',
 'a published map\'s content, the installed folder of the mod alongside', 'Update to the installed version',
 'a stock object placed that a mod of editor tweaks changes', 'Remove from map of the mod used by the whole map',
 'a map that uses Snapmap+\'s own features (its entry is no mod)'].forEach(n =>
  assert.ok(cases.some(c => c.name === n), 'the cases have the step: ' + n));
const pub = cases.filter(c => /published map's content/.test(c.name))[0];
assert.ok(pub.save.some(r => r.pinned) && pub.map.mods.some(m => m.pinned && m.key === 'alex.r2-imp'),
          'the published step pins the row, and the installed folder is the map\'s mod');

console.log('mods_inmap_test: OK (' + cases.length + ' steps, ' + checked + ' mod states)');
