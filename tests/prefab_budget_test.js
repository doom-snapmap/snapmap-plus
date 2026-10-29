/* The Prefabs tab's Limits arithmetic (prefab_budget.js) against the live-measured editor rows of build
   13954591, plus the page contract: one scene parse shared with the preview, Limits under the preview,
   requests only for the selected prefab. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
const budget = require(path.join(root, 'src', 'ui', 'webview', 'prefab_budget.js'));
const html = fs.readFileSync(path.join(root, 'src', 'ui', 'webview', 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const viewport = fs.readFileSync(path.join(root, 'src', 'ui', 'webview', 'prefab_viewport.js'), 'utf8');
const host = fs.readFileSync(path.join(root, 'src', 'ui', 'webview', 'snapmap_plus_ui_webview.cpp'), 'utf8');
let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }
function same(a, b) { return JSON.stringify(a) === JSON.stringify(b); }
function near(a, b) { return Math.abs(a - b) < 1e-9; }

const LIMITS = [768, 14284, 10384, 2038, 672, 1000, 8192, 8192, 0, 0, 500000, 64];
const USAGE = [32, 67, 91, 1, 30, 0, 1, 0, 1552, 12, 2545, 0];   /* test1 baseline, R-002 */
const C90 = 'maps/modules/ind_connections/c_ind_90.decl';
const table = {
  gen: 3, limits: LIMITS, usage: USAGE, reached: 0,
  layers: {autoView: 0x20000, sound: 0x400000, source: 'decl'},
  sedefs: {
    'unknown/unknown': {l: [0, 1, 2, 0, 1, 0, 0, 0, 0, 0, 50, 0], auto: 0, ns: 0},
    'custom/timeline': {l: [0, 1, 2, 0, 1, 0, 0, 0, 0, 0, 50, 0], auto: 1, ns: 0},
    'demons/imp_enc': {l: [1, 1, 2, 0, 1, 0, 0, 0, 0, 0, 1500, 1], auto: 0, ns: 0},
    'prop/new/crate01': {l: [1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 25, 0], auto: 0, ns: 0},
    'volume/blocking': {l: [0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 10, 0], auto: 0, ns: 1},
    'fx/decal': {l: [0, 1, 2, 0, 0, 0, 0, 1, 0, 0, 25, 0], auto: 0, ns: 0},
    'light/point': {l: [0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 50, 0], auto: 3, ns: 0},
    'visblockers/cap': {l: [1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 25, 0], auto: 0, ns: 0}
  },
  inherits: {
    'snapmaps/unknown': 'unknown/unknown',
    'snapmaps/placeablesnapaiencounter/imp': 'demons/imp_enc',
    'snapmaps/props/new/crate01': 'prop/new/crate01',
    'snapmaps/volume/blocking': 'volume/blocking',
    'snapmaps/fx/decal_coop': 'fx/decal',
    'snapmaps/light/dynamic_point': 'light/point',
    'snapmaps/visblockers/industrial/cap_04': 'visblockers/cap'
  },
  modules: {},
  aliases: {c_ind_90: C90}
};
/* c_ind_90 as read live: 1 floor, 8 walls, 2 portals; card KB 2926/808/50; GAME soundenv, spawnnode,
   3 flight (template block 50 + 250 KB, 2 entities); VIEW light/probe, light/point, speaker on the
   filter mask 0x420001. */
table.modules[C90] = {own: [0, 0, 33, 0, 0, 0, 0, 0, 2926, 808, 50, 0], templ: [0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 300, 0],
                      view: [0, 3, 6, 1, 1, 0, 0, 0, 0, 0, 150, 0], mask: 0x420001,
                      parts: [[0x1, 0, [0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 250, 0]],
                              [0x20000, 1, [0, 2, 4, 1, 0, 0, 0, 0, 0, 0, 100, 0]],
                              [0x400000, 1, [0, 1, 2, 0, 1, 0, 0, 0, 0, 0, 50, 0]],
                              [0x4, 1, [0, 4, 8, 4, 0, 0, 0, 0, 0, 0, 200, 0]],
                              [0x800000, 1, [0, 7, 14, 0, 0, 0, 0, 7, 0, 0, 175, 0]],
                              [0x2, 2, [0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0]]]};

function ent(inherit, className, edit) {
  return {entityDef: {className: className || 'idEntity', inherit: inherit, state: {edit: edit || {}}}};
}

/* Single rows reproduce the measured placement deltas of R-002. */
check(same(budget.sum([ent('snapmaps/placeablesnapaiencounter/imp')], [], table).block, [1, 1, 2, 0, 1, 0, 0, 0, 0, 0, 1500, 1]), 'imp_enc');
check(same(budget.sum([ent('snapmaps/props/new/crate01')], [], table).block, [1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 25, 0]), 'crate01');
check(same(budget.sum([ent('snapmaps/volume/blocking', 'idVolume_Blocking')], [], table).block, [0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 10, 0]),
      'a Blocking Box is network-static by default');
check(same(budget.sum([ent('snapmaps/volume/blocking', 'idVolume_Blocking', {isNetworkStatic: false})], [], table).block,
           [0, 1, 1, 0, 1, 0, 0, 0, 0, 0, 10, 0]), 'an explicit isNetworkStatic false restores the network cost');
check(same(budget.sum([ent('snapmaps/props/new/crate01', 'idProp_Coop', {isNetworkStatic: true})], [], table).block,
           [1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 25, 0]), 'an explicit isNetworkStatic true removes it');
check(same(budget.sum([ent('snapmaps/unknown', 'idTarget_Timeline')], [], table).block, [0, 1, 2, 0, 1, 0, 0, 0, 0, 0, 50, 0]),
      'a timeline binds to the Timeline tile');

/* An object prefab: the column sum, nothing per prefab. */
const objects = [ent('snapmaps/placeablesnapaiencounter/imp'), ent('snapmaps/props/new/crate01'),
                 ent('snapmaps/volume/blocking', 'idVolume_Blocking'), ent('snapmaps/fx/decal_coop')];
const obj = budget.compute(objects, [], table);
check(same(obj.block, [2, 4, 6, 0, 2, 0, 0, 1, 0, 0, 1560, 1]), 'object prefab block');
check(!obj.misses.length && !obj.unknownModules.length, 'object prefab has no unknowns');

/* A palette miss costs 1 entity, 1 render model, 1 network and is named once. */
const miss = budget.compute([ent('snapmaps/not/a/row'), ent('snapmaps/not/a/row'), {}], [], table);
check(same(miss.block, [0, 3, 3, 0, 3, 0, 0, 0, 0, 0, 0, 0]), 'misses cost 1/1/1 each');
check(same(miss.misses, ['snapmaps/not/a/row', '(no inherit)']), 'misses are listed once');

/* A module prefab: own + template + view + the bucket's 50 KB, plus the scene's own entities. */
const mod = budget.compute([ent('snapmaps/visblockers/industrial/cap_04', 'idSnapMapCapEntity'),
                            ent('snapmaps/light/dynamic_point', 'idLight')],
                           [C90], table, null, {skipView: true});
check(same(mod.block, [1, 6, 40, 1, 1, 0, 0, 0, 2926, 808, 575, 0]), 'module prefab block, VIEW entity counted once');
const modNoSkip = budget.sum([ent('snapmaps/light/dynamic_point', 'idLight')], [C90], table);
check(modNoSkip.block[3] === 2, 'without skipView a VIEW row in the scene counts again');
check(same(budget.sum([], ['c_ind_90'], table).block, budget.sum([], [C90], table).block), 'module found by leaf');
check(same(budget.sum([], ['0_c_ind_90'], table).block, budget.sum([], [C90], table).block), 'module found by a numbered leaf');
const unk = budget.compute([], ['maps/modules/x/not_resident.decl', 'maps/modules/x/not_resident.decl'], table);
check(same(unk.unknownModules, ['maps/modules/x/not_resident.decl']) && same(unk.block, [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
      'an unknown module is reported once and adds nothing');
check(budget.leaf('maps/modules/ind_dlc/ind_totally_blank_room_4x.decl') === 'ind_totally_blank_room_4x', 'leaf');

/* A module prefab file: each entity filtered by its instance's saved layerMask | autoView, and the
   module's template groups filtered by the same mask (live: a door on the disabled layer 0x1000000
   is not counted). */
const fileDoc = {
  instances: [{moduleName: C90, layerMask: 1}, {moduleName: C90, layerMask: 0x400001}],
  instanceEntities: {keyValues: [0, 2, 3, 4], values: [10, 11, 12, 13]},
  entities: [Object.assign(ent('snapmaps/visblockers/industrial/cap_04', 'idSnapMapCapEntity'), {uniqueId: 10, layerMask: 1}),
             Object.assign(ent('snapmaps/props/new/crate01'), {uniqueId: 11, layerMask: 0x1000000}),
             Object.assign(ent('snapmaps/props/new/crate01'), {uniqueId: 12, layerMask: 0x400000}),
             Object.assign(ent('snapmaps/props/new/crate01'), {uniqueId: 13, layerMask: 0x1000000})]
};
const ms = budget.moduleScene(fileDoc);
check(same(ms.entityModule, [0, 0, 1, -1]) && ms.modules[1].layerMask === 0x400001, 'module scene membership');
const filtered = budget.sum(ms.entities, ms.modules, table, {skipView: true, entityModule: ms.entityModule});
/* instance 0 (mask 1): own 33 + groups 0x1 and 0x20000 + bucket; instance 1 adds the 0x400000 group. */
const inst0 = [0, 4, 37, 1, 0, 0, 0, 0, 2926, 808, 50 + 50 + 250 + 100 + 50, 0];
const inst1 = [0, 5, 39, 1, 1, 0, 0, 0, 2926, 808, 50 + 50 + 250 + 100 + 50 + 50, 0];
const cap = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 25, 0], crate = [1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 25, 0];
const want = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
[inst0, inst1, cap, crate, crate].forEach(function (b) { for (let i = 0; i < 12; i++) want[i] += b[i]; });
check(same(filtered.block, want), 'layer filters: the disabled-layer crate in a module drops, the global one counts');
check(filtered.filtered === 1, 'one entity filtered out');
check(budget.sum([Object.assign(ent('snapmaps/props/new/crate01'), {layerMask: 0x1000000})], [], table).filtered === 1,
      'an object prefab entity on a layer no room counts is dropped');
check(same(budget.sum([], [{name: C90}], table).block, budget.sum([], [C90], table).block),
      'without a saved mask the palette default blocks are used');

/* Meter formulas: the HUD ratios, R-002 baseline. */
const r = budget.meterRatios(USAGE, LIMITS);
check(near(r.network, 30 / 672), 'NETWORK = network / limit');
check(near(r.objects, (1552 + 12 + 2545) / 500000), 'OBJECTS = max(entities, KB sum over the KB limit sum)');
check(near(r.memory, 91 / 10384), 'MEMORY = max(models, lights, world areas, decals)');
check(r.demons === 0 && r.decals === 0, 'DEMONS and DECALS');
const ents = budget.meterRatios([0, 14284, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], LIMITS);
check(ents.objects === 1, 'OBJECTS takes the entity ratio when it is larger');
const lights = budget.meterRatios([0, 0, 10, 2038, 0, 0, 1, 0, 0, 0, 0, 0], LIMITS);
check(lights.memory === 1, 'MEMORY takes the light ratio when it is larger');
check(near(obj.meters.demons.add, 1 / 64) && near(obj.meters.network.before, 30 / 672) &&
      near(obj.meters.network.after, 32 / 672), 'meter add, before and after');
check(near(mod.meters.objects.after, (1552 + 12 + 2545 + 2926 + 808 + 575) / 500000), 'objects after a module');

/* The reached rule: network, objects, memory at 1.0; demons only past the limit. */
check(!budget.reachedBy({network: 0.99, objects: 0, memory: 0, demons: 1}), '64 demons are allowed');
check(budget.reachedBy({network: 0, objects: 0, memory: 0, demons: 65 / 64}), '65 demons reach the limit');
check(budget.reachedBy({network: 1, objects: 0, memory: 0, demons: 0}), 'network at the limit');
check(budget.reachedBy({network: 0, objects: 1, memory: 0, demons: 0}), 'objects at the limit');
check(budget.reachedBy({network: 0, objects: 0, memory: 1, demons: 0}), 'memory at the limit');
const full = budget.compute([ent('snapmaps/props/new/crate01')], [], table,
                            {limits: LIMITS, usage: [0, 0, 0, 0, 671, 0, 0, 0, 0, 0, 0, 64]});
check(full.reached && full.meters.network.reached && !full.meters.demons.reached, 'the 672nd network object reaches the limit');
const demons = budget.compute([ent('snapmaps/placeablesnapaiencounter/imp')], [], table,
                              {limits: LIMITS, usage: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64]});
check(demons.reached && demons.meters.demons.reached, 'the 65th demon reaches the limit');

/* Formatting and the bounded memo. */
check(budget.formatPercent(30 / 672) === '4.5%' && budget.formatPercent(0) === '0.0%' &&
      budget.formatPercent(1 / 8192) === '<0.1%' && budget.formatPercent(Infinity) === 'over', 'percent format');
check(budget.rawText('objects', obj.block) === '4 entities, 1560 KB memory', 'raw tooltip text');
const cache = budget.createCache(2);
cache.set('a', 1); cache.set('b', 2); cache.get('a'); cache.set('c', 3);
check(cache.get('a') === 1 && cache.get('b') === undefined && cache.get('c') === 3 && cache.size() === 2, 'LRU cache');

/* Page contract. */
check(html.includes('id="pcLimits"') && html.indexOf('id="pcPreview"') < html.indexOf('id="pcLimits"') &&
      html.indexOf('id="pcLimits"') < html.indexOf('for="pcName"'), 'Limits sits under the preview, above Name');
check(html.includes('>Limits</div>') && html.includes('id="pcLimitsDetails"') && html.includes('<summary>Details</summary>'),
      'Limits heading and collapsible Details');
check(html.indexOf('src="prefab_budget.js"') > html.indexOf('src="prefab_transform.js"'), 'the module is loaded');
const detail = html.slice(html.indexOf('function renderPrefabDetail(d)'), html.indexOf('function pfShowFootprint(meta)'));
check((detail.match(/parseScene\(/g) || []).length === 1, 'the selected scene is parsed once');
check(detail.includes('prefabViewport.load(d.scene || \'\', doc)') && detail.includes('countNavigationBoxes(doc)'),
      'the preview and the AI Navigation count reuse the parsed scene');
check(detail.includes("post({cmd:'budgetUsage'})"), 'usage is asked for on each selection');
check(html.includes("if (!pfBudgetAsked) { pfBudgetAsked = true; post({cmd:'budgetTable'}); }"), 'the table is asked for once');
check((html.match(/prefabBudget\.sum\(/g) || []).length === 1 &&
      html.indexOf('prefabBudget.sum(') > html.indexOf('function pfRenderLimits()'), 'only the selected prefab is summed');
check(viewport.includes('function load(sceneText, parsedDoc)'), 'the viewport accepts a parsed scene');
check(host.includes('cmd == L"budgetTable"') && host.includes('cmd == L"budgetUsage"') && host.includes('poc_budget_poll();'),
      'host routes budget requests and re-sends the table after a rebuild');
check(host.includes('\\"sig\\":\\""'), 'prefabDetail carries a content signature');

if (failures) { console.error(failures + ' prefab budget check(s) failed'); process.exit(1); }
console.log('prefab_budget_test: OK');
