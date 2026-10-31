/* The Prefabs tab's Create gate, Load / Place outcome toasts, kind classification and live Limits,
   lifted from the page and run, plus the layout and host contracts behind them. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
const read = f => fs.readFileSync(path.join(root, 'src', 'ui', 'webview', f), 'utf8').replace(/\r\n/g, '\n');
const html = read('mockup.html'), css = read('studio.css'), host = read('snapmap_plus_ui_webview.cpp');
const budget = require(path.join(root, 'src', 'ui', 'webview', 'prefab_budget.js'));
let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }

/* A top-level page function's source, by brace matching from its declaration. */
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
function between(begin, end) {
  const b = html.indexOf(begin), e = html.indexOf(end);
  check(b > 0 && e > b, begin + ' is marked for extraction');
  return html.slice(b, e);
}

/* ---- Create follows the live selection ---- */
{
  const api = {};
  new Function('api', between('  // --- create gate ---', '  // --- create gate end ---') + '\napi.gate = pfCreateGate;')(api);
  const objects = {known: true, entityMode: 1, blueprint: 0, modules: 0};
  const blueprint = n => ({known: true, entityMode: 0, blueprint: 1, modules: n});
  let g = api.gate(objects, 0);
  check(!g.enabled && g.title === 'Select entities first' && !g.modules, 'Object mode with nothing selected: disabled, "Select entities first"');
  g = api.gate(objects, 3);
  check(g.enabled && g.title === 'Create from Selection (3 selected)', 'Object mode with a selection: enabled at once');
  g = api.gate(blueprint(0), 3);
  check(!g.enabled && g.modules && g.title === 'Select modules first',
        'Blueprint with no module selected: disabled, "Select modules first" (entities do not count there)');
  g = api.gate(blueprint(2), 0);
  check(g.enabled && g.modules && /2 modules selected in Blueprint/.test(g.title), 'Blueprint with modules selected: enabled');
  g = api.gate(blueprint(1), 0);
  check(g.enabled && /1 module selected/.test(g.title), 'one module reads in the singular');
  g = api.gate(blueprint(-1), 0);
  check(g.enabled, 'a backend that cannot count modules leaves the gate to the engine');
  g = api.gate({known: false, entityMode: 0, blueprint: 0}, 0);
  check(!g.enabled && g.title === 'Select entities first', 'before the first editorMode the entity count decides');
  const update = lift('updateCreateBtn');
  check(update.includes("getElementById('npCreate')") && update.includes('c.disabled = !ok') &&
        update.includes("'Select modules first' : 'Select entities first'"),
        'the Create dialog confirm follows the same gate with the same tooltip');
  check(html.includes("modules: typeof d.modules === 'number' ? d.modules : -1") && /else if \(d\.kind === 'selCount'\)[\s\S]{0,120}updateCreateBtn\(\)/.test(html),
        'editorMode carries the module count and selCount re-gates Create');
  const click = html.slice(html.indexOf("if (pcCreateBtn) pcCreateBtn.addEventListener('click'"), html.indexOf("var npCancel = document.getElementById('npCancel')"));
  check(click.includes('if (!pfCreateGate(edMode, editorSelCount).enabled) return;'), 'a disabled Create opens nothing');
  check(html.includes('if (npCreate.disabled) return;'), 'a disabled confirm creates nothing');
}

/* ---- Create's refusals name the true cause ---- */
{
  const res = html.slice(html.indexOf("} else if (d.kind === 'createPrefabResult') {"), html.indexOf("} else if (d.kind === 'setDisplayNamesResult') {"));
  check(res.includes("d.result === 4) toast('The cursor is over an entity that is not selected -- hover one of the selected entities in the 3D view, then try again'"),
        'a hovered entity that is not selected is reported as such, not as an empty selection');
  check(res.includes('d.result === 5)'), 'the copy check refusing a real selection has its own message');
  const create = host.slice(host.indexOf('static void poc_apply_create_prefab()'), host.indexOf('static bool poc_prefab_dir('));
  check(create.includes('sh_prefab_host::create_precheck(sel, count, hovered, have_hover)') &&
        create.indexOf('create_precheck') < create.indexOf('poc_serialize_selection_raw('),
        'the host tells an empty selection from an unselected hover before it serializes');
  check(create.includes('g_create_result = 5;'), 'a serialize that returns nothing after the precheck is result 5');
}

/* ---- Load / Place says what is true ---- */
{
  const api = {};
  new Function('api', between('  // --- place toast ---', '  // --- place toast end ---') + '\napi.toast = pfPlaceToast;')(api);
  let t = api.toast({result: 1, name: 'Stairs', placed: 'queued', reason: ''});
  check(t.text === '"Stairs" is in hand: position it and press Space, or Esc to cancel' && t.level === 'ok',
        'a queued paste says the prefab is in hand and Space places it');
  check(!/Ctrl\+V/.test(t.text), 'no Ctrl+V when the paste is queued');
  t = api.toast({result: 1, name: 'Stairs', placed: 'staged', reason: 'the editor is not in Object mode'});
  check(t.text === '"Stairs" is staged but not in hand because the editor is not in Object mode: press Ctrl+V in the 3D view to place it' &&
        t.level === 'warn', 'staged only: Ctrl+V with the reason');
  t = api.toast({result: 1, name: 'Stairs', placed: 'staged', reason: 'the cursor was over an entity, where the editor does not paste'});
  check(/move the cursor off the entity, then press Ctrl\+V/.test(t.text), 'the hover reason says to move the cursor first');
  t = api.toast({result: 1, name: 'Stairs', placed: 'unknown'});
  check(/was sent to the editor; if it is not in hand, press Ctrl\+V/.test(t.text), 'no decision in time: says only what is known');
  check(api.toast({result: -1, name: 'X'}).level === 'err' && api.toast({result: 0, name: 'X'}).level === 'err', 'failures stay errors');
  check(!html.includes("staged -- press Ctrl+V in the 3D view to place it'"), 'the old always-Ctrl+V toast is gone');
  check(host.includes('static void poc_poll_place_outcome()') && host.includes('poc_poll_place_outcome();') &&
        host.includes('&& !g_place_wait) {'),
        'the host posts an object placement once the game thread has decided it');
}

/* ---- A prefab's kind is its file's ~type ---- */
{
  const api = {};
  new Function('api', 'var prefabBudget = api.budget;\n' + between('  // --- prefab kind ---', '  // --- prefab kind end ---') +
    '\napi.kind = pfKindOfDetail; api.summary = pfModuleSummary;')(Object.assign(api, {budget}));
  check(api.kind({prefabKind: 'modules'}, {}) === 'modules', 'an idSnapMap without a block is a module prefab');
  check(api.kind({prefabKind: 'objects'}, {kind: 'modules'}) === 'objects', 'an idSnapEntityPrefab is an object prefab whatever its block says');
  check(api.kind({}, {kind: 'modules'}) === 'modules' && api.kind({}, {}) === 'objects', 'a message without a kind falls back to the view');
  const doc = {instances: [{moduleName: 'a'}, {moduleName: 'b'}], entities: [{}, {}, {}]};
  const s = api.summary({}, doc, 0);
  check(s.moduleCount === 2 && s.entityCount === 3, 'a module prefab without a summary counts its modules and objects from the scene');
  const kept = {moduleCount: 4, entityCount: 9, modules: [{}]};
  check(api.summary(kept, doc, 0) === kept, 'a saved summary is used as it is');
  check(html.includes('selectedPrefabKind = pfKindOfDetail(d, meta);') && html.includes('pfShowFootprint(pfModuleSummary(meta, doc, d.count));'),
        'Prefab Details decides the kind and the footprint counts through them');
  check(host.includes('json += modules ? L",\\"prefabKind\\":\\"modules\\"" : L",\\"prefabKind\\":\\"objects\\"";'),
        'the host always sends the kind it read');
  const ls = host.indexOf('static void poc_send_prefabs()\n{'), list = host.slice(ls, host.indexOf('\n}\n', ls));
  check(list.includes('sh_prefab_host::kind_of_type(poc_file_top_type(path))'), 'the list classifies by ~type too');
}

/* ---- Live Limits, with and without a held prefab ---- */
{
  const LIMITS = [768, 14284, 10384, 2038, 672, 1000, 8192, 8192, 0, 0, 500000, 64];
  const USAGE = [32, 67, 91, 1, 30, 0, 1, 0, 1552, 12, 2545, 0];
  const add = [1, 2, 4, 0, 1, 0, 0, 0, 0, 0, 100, 0];   /* Corpse Dropper, R-006 */
  const held = USAGE.map((v, i) => v + add[i]);         /* usage held = usage after Space */
  const free = budget.meters(add, USAGE, LIMITS);
  const hold = budget.meters(add, held, LIMITS, true);
  check(free.after.join() === held.join(), 'not held: Map after is the usage plus this prefab');
  check(hold.after.join() === held.join(), 'held: Map after is the live usage, which already counts it');
  check(budget.meters(add, held, LIMITS, false).after.join() !== held.join(), '(without the rule the held prefab would count twice)');
  budget.METERS.forEach(k => {
    check(hold.meters[k].before === hold.meters[k].after && hold.meters[k].held,
          'held ' + k + ': the bar\'s current part grows to Map after');
    check(hold.meters[k].add === free.meters[k].add, 'held ' + k + ': This prefab still shows its share');
    check(Math.abs(hold.meters[k].after - free.meters[k].after) < 1e-12, 'held ' + k + ': the same Map after as before the paste');
  });
  const api = {};
  new Function('api', lift('pfBudgetIsHeld') + '\napi.isHeld = pfBudgetIsHeld;')(api);
  check(api.isHeld({name: 'Stairs', folder: ''}, {name: 'Stairs', folder: ''}), 'the selected prefab held');
  check(!api.isHeld({name: 'Stairs', folder: 'a'}, {name: 'Stairs', folder: ''}), 'the same name in another folder is another prefab');
  check(!api.isHeld(null, {name: 'Stairs', folder: ''}) && !api.isHeld({name: 'Other', folder: ''}, {name: 'Stairs', folder: ''}),
        'nothing held, or another prefab held: the ordinary rule');
  const handler = html.slice(html.indexOf("} else if (d.kind === 'budgetUsage') {"), html.indexOf("} else if (d.kind === 'prefabModelResolved') {"));
  check(handler.includes('pfBudgetHeld = d.held') && handler.includes('pfRenderLimits();'), 'each budgetUsage re-renders the rows and bars');
  const render = lift('pfRenderLimits');
  check(render.includes('var held = pfBudgetIsHeld(pfBudgetHeld, pfBudgetSel);') &&
        render.includes('prefabBudget.meters(s.block, live.usage, live.limits, held)'), 'the rows use the held rule');
  check(lift('pfLimitsBar').includes('!v.held'), 'a held prefab draws no extra sliver past the bar');
  check(host.includes('poc_budget_usage_poll();') && host.includes('g_usage_gate.changed(body, sh_webview_json::to_utf8(held))'),
        'the host re-posts usage from the think loop only when it or the held prefab changed');
  check(!/SetTimer|setInterval/.test(host.slice(host.indexOf('static void poc_budget_usage_poll()'), host.indexOf('static void poc_budget_usage_poll()') + 600)),
        'no fixed timer posts unchanged values');
}

/* ---- Legend and Limits table fit ---- */
{
  check(/@media \(max-height: 520px\) \{[^}]*\.pc-preview \{ flex-basis: 130px; \}[\s\S]*?\.pc-preview\.pc-legend-open \.pc-preview-tag \{ visibility: hidden; \}/.test(css),
        'at the smallest preview the open legend hides the status line instead of covering it');
  check(/\.pc-legend \{[^}]*max-height: calc\(100% - 12px\);/.test(css) && /\.pc-preview-legend \{[^}]*min-height: 0; overflow: auto;/.test(css),
        'the legend never leaves the preview');
  check(lift('pfLegendSetOpen').includes("classList.toggle('pc-legend-open', open)"), 'the preview knows when the legend is open');
  const cols = /\.pc-limits-table \{[^}]*grid-template-columns: ([^;]+);[^}]*column-gap: (\d+)px;/.exec(css);
  check(!!cols, 'the Limits grid columns are readable');
  if (cols) {
    const mins = cols[1].match(/minmax\((\d+)px,/g) || [];
    const sum = mins.reduce((a, m) => a + Number(/\d+/.exec(m)[0]), 0) + 3 * Number(cols[2]);
    check(mins.length === 4, 'every column can shrink');
    /* the 297px pane, less the card's padding (measured 282px of content) */
    check(sum <= 282, 'the table fits the narrowest pane: ' + sum + 'px of minimums');
  }
}

if (failures) { console.error(failures + ' prefab details check(s) failed'); process.exit(1); }
console.log('prefab_details_test: OK');
