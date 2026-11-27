/* The Prefabs tab's mod status (backend/prefab_mods.c), list pill, details rows, Load / Place answers and Export / Import
   buttons, lifted from the shipped HTML, plus the host's side of each message. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const web = path.join(__dirname, '..', 'src', 'ui', 'webview');
const src = fs.readFileSync(path.join(web, 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const host = fs.readFileSync(path.join(web, 'snapmap_plus_ui_webview.cpp'), 'utf8').replace(/\r\n/g, '\n');
const css = fs.readFileSync(path.join(web, 'studio.css'), 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) {
  if (!ok) { console.error('[FAIL] ' + message); failures++; }
}

const begin = src.indexOf('  // --- prefab mods (pure) ---');
const end = src.indexOf('  // --- prefab mods (pure) end ---', begin);
if (begin < 0 || end < 0) throw new Error('could not extract the prefab mods logic');
const api = {};
new Function('api', src.slice(begin, end) +
  '\napi.parse = pfModsParse; api.badge = pfModsBadge; api.detail = pfModsDetail; api.loadToast = pfModsLoadToast;' +
  ' api.fileToast = pfFileResultToast; api.label = pfModLabel;')(api);

/* MODS_STATUS uses "installed" for available content from either source. */
const door = {id: 'test.door', salt: 'testdoor', build: 'a'.repeat(64), size: 100, installed: true, state: 'served', cause: ''};
const ghost = {id: 'ghost.mod', salt: 'ghostmod', build: '9'.repeat(64), size: 1048576, installed: false, state: 'missing', cause: ''};
const both = JSON.stringify({count: 2, missing: 1, mods: [door, ghost]});

/* Parsing: the host's escaped string or an object; anything malformed is no mods. */
let st = api.parse(both);
check(st.count === 2 && st.missing === 1 && st.mods[1].id === 'ghost.mod', 'status string parses: ' + JSON.stringify(st));
check(api.parse({mods: [door]}).count === 1 && api.parse({mods: [door]}).missing === 0, 'an object parses too');
check(api.parse('').count === 0 && api.parse('{not json').count === 0 && api.parse(null).count === 0, 'malformed reads as none');
check(api.parse({mods: [{id: ''}, null, {salt: 'x'}]}).count === 0, 'rows without an id are dropped');
check(api.parse({mods: [{id: 'x'}]}).missing === 1, 'a row without installed:true counts as missing');
check(api.label(door) === 'test.door' && api.label({id: 'x'}) === 'x', 'a label is the mod\'s name (a mod has no version)');
check(api.label({id: 'abcd1234', salt: 'abcd1234'}) === 'an unnamed mod', 'a mod recorded without a name is never named by its salt');
check(api.badge({count: 1, missing: 1, mods: [{id: 'abcd1234', salt: 'abcd1234', installed: false}]}).text === 'Needs mods: an unnamed mod',
      'nor in its pill');

/* The list pill. */
check(api.badge(api.parse('')) === null, 'no mods, no pill');
let b = api.badge(st);
check(b && b.text === 'Needs mods: ghost.mod' && /pf-mods-missing/.test(b.cls), 'a missing mod names it: ' + JSON.stringify(b));
check(b && b.title === 'test.door (content ready)\nghost.mod (missing content)', 'the title lists every mod and its state');
b = api.badge(api.parse({mods: [door]}));
check(b && b.text === 'Mods: test.door' && b.cls === 'pf-mods', 'installed mods are listed: ' + JSON.stringify(b));
b = api.badge(api.parse({mods: [door, Object.assign({}, door, {id: 'other.mod'})]}));
check(b && b.text === 'Mods: test.door +1', 'more mods are counted');
b = api.badge(api.parse({mods: [ghost, Object.assign({}, ghost, {id: 'g2'}), door]}));
check(b && b.text === 'Needs mods: ghost.mod +1', 'only the missing ones are named when any is missing');

/* Prefab Details. */
let d = api.detail(st);
check(d.rows.length === 2 && d.rows[0].label === 'test.door' && d.rows[0].state === 'Content ready' && !d.rows[0].missing &&
      d.rows[1].state === 'Missing content' && d.rows[1].missing, 'one row per mod with its state');
const importHelp = "In the Prefabs tab, use Import to open the author's prefab ZIP. Then select the imported prefab and choose Load / Place.";
check(d.note === importHelp, 'missing content directs the player to prefab import');
/* A mod and the open map, in the Mods tab's words (its mapStateOf, by the mod's namespace). */
const inMap = salt => ({state: salt === door.salt ? 'objects' : 'out'});
d = api.detail(api.parse({mods: [door, Object.assign({}, door, {id: 'other.mod', salt: 'othermod'}), ghost]}), inMap);
check(d.rows[0].state === 'In this map' && d.rows[0].inMap && d.rows[1].state === 'Not in this map' && !d.rows[1].inMap &&
      d.rows[2].state === 'Missing content' && d.rows[2].missing, 'with a map open, each mod says whether the map uses it');
check(api.detail(api.parse({mods: [door]}), () => null).rows[0].state === 'Content ready', 'no map: locally available content');
b = api.badge(api.parse({mods: [door, ghost]}), inMap);
check(b && b.title === 'test.door (in this map)\nghost.mod (missing content)', 'the pill title says the same: ' + JSON.stringify(b && b.title));
check(api.detail(api.parse({mods: [ghost, ghost]})).note === importHelp, 'one action for several missing mods');
check(api.detail(api.parse({mods: [door]})).note === 'All mod content this prefab needs is available.', 'cache-only content is ready');
const incomplete = Object.assign({}, ghost, {state: 'incomplete', cause: 'The folder is missing an object.'});
d = api.detail(api.parse({mods: [incomplete]}), () => ({state: 'whole'}));
check(d.rows[0].state === 'Content incomplete' && d.rows[0].missing && !d.rows[0].inMap && d.note === importHelp,
      'a mod in the map may still lack an object this prefab needs');
check(api.detail(api.parse('')).rows.length === 0 && api.detail(api.parse('')).note === '', 'no mods, no section');

/* Load / Place answers about mods. */
let t = api.loadToast({result: 3, name: 'Door', message: 'Preparing mod content for Door.'});
check(t && t.level === 'ok' && t.text === 'Preparing mod content for Door.', 'held: the message');
t = api.loadToast({result: -7, name: 'Door', message: ''});
check(t && t.level === 'warn' && t.text === '"Door" could not be placed in this map.', 'refused without text');
t = api.loadToast({result: -7, name: 'Door', message: 'Another prefab is waiting for its mods.'});
check(t && t.text === 'Another prefab is waiting for its mods.', 'refused with the backend text');
check(api.loadToast({result: 1, name: 'Door'}) === null && api.loadToast({result: -6}) === null, 'other answers keep their toasts');

/* Export and Import answers. */
check(api.fileToast({op: 'export', cancelled: true}) === null, 'a cancelled dialog says nothing');
t = api.fileToast({op: 'export', ok: true, message: 'Exported prefab Door with 1 mod to C:\\x\\Door.zip (2 KB).'});
check(t && t.level === 'ok' && /Exported prefab Door/.test(t.text), 'export success');
t = api.fileToast({op: 'import', ok: false, message: ''});
check(t && t.level === 'err' && t.text === 'The prefab was not imported', 'import failure without text');

/* The page wiring. */
check(/id="prefabExportBtn"[^>]*disabled/.test(src) && /id="prefabImportBtn"/.test(src), 'Export and Import buttons exist');
check(src.indexOf("post({cmd:'exportPrefab', name: name, folder: selectedPrefabFolder})") !== -1, 'Export posts exportPrefab');
check(src.indexOf("post({cmd:'importPrefab'})") !== -1, 'Import posts importPrefab');
check(src.indexOf("['prefabExportBtn', 'prefabDeleteBtn', 'prefabLoadBtn']") !== -1, 'Export follows the selection');
check(/id="pcMods"[\s\S]*id="pcModsRows"[\s\S]*id="pcModsNote"/.test(src), 'Prefab Details has the Mods section');
check(src.indexOf('pfModsShown = pfModsParse(d.modsStatus);') !== -1 && src.indexOf('pfShowMods(pfModsShown)') !== -1, 'details render the mods status');
check(src.indexOf('var mbadge = pfModsBadge(prefabMods[') !== -1, 'the list row shows the pill');
check(src.indexOf('var dmods = d.mods && typeof d.mods') !== -1 && src.indexOf("['mods', 'needs mods']") !== -1,
      'the list reads the mods map and the filter finds them');
const lp = src.indexOf("d.kind === 'loadPrefabResult'");
check(lp !== -1 && src.indexOf('var pml = pfModsLoadToast(d);', lp) !== -1 &&
      src.indexOf('var pml = pfModsLoadToast(d);', lp) < src.indexOf("d.prefabKind === 'modules'", lp),
      'mods answers are told before the kind-specific toasts');
check(src.indexOf("d.kind === 'prefabFileResult'") !== -1, 'Export and Import answers are handled');
/* Deliver real list messages twice to the page, keeping a same-named prefab in another folder missing. */
const listStart = src.indexOf("d.kind === 'prefabs') {") + "d.kind === 'prefabs') {".length;
const listEnd = src.indexOf("} else if (d.kind === 'prefabFileResult')", listStart);
const deliver = new Function('d', 'pfModsParse', 'selectedPrefabName', 'selectedPrefabFolder', 'pfShowMods', 'syncModsInterest',
  'var prefabTags, prefabKinds, prefabMods, pfListLast, pfModsShown; function renderPrefabList() {}' +
  src.slice(listStart, listEnd) + '\nreturn prefabTags;');
const observed = [];
const listMods = {Door: {mods: [ghost]}, 'Hangar/Door': {mods: [ghost]}};
let interestUpdates = 0;
const refresh = () => deliver({mods: listMods}, api.parse, 'Door', 'Hangar', st => observed.push(api.detail(st)),
  () => { interestUpdates++; });
let tags = refresh();
check(tags['Hangar/Door'].includes('needs mods') && observed[0].note === importHelp, 'selected prefab starts missing');
listMods['Hangar/Door'] = {mods: [door]};
tags = refresh();
check(interestUpdates === 2, 'each delivered prefab list updates the map subscription');
check(observed.length === 2 && observed[1].rows[0].state === 'Content ready' && tags.Door.includes('needs mods') &&
      !tags['Hangar/Door'].includes('needs mods'), 'reimport updates the selected folder and the missing-content filter');
check(/if \(kind == 3 && r >= 0\) ui_send_prefabs\(\);/.test(host),
      'host refreshes even when an identical prefab gains only its cached content');
/* A module prefab's missing-module warning is shown. */
check(src.indexOf("pfShowModuleCheck(selectedPrefabKind === 'modules' ? d.moduleCheck : null)") !== -1 && /id="pcNeeds"/.test(src),
      'the module check warning is kept');

/* Load / Place carries the map that the page displayed at the click. */
const placeFn = src.slice(src.indexOf('  function pfLoadPlace('), src.indexOf('  if (prefabLoadBtn)'));
const send = new Function('edMode', 'post', placeFn + '\npfLoadPlace("Door", "", "objects");');
let clicked;
const mode = {known: true, context: 27};
send(mode, msg => { clicked = msg; });
mode.context++;
check(clicked.context === 27 && clicked.cmd === 'loadPrefab', 'the queued click keeps the original map');
check(host.includes('call.editor_context == g_editor_context_last') && host.includes('g_mods_wait_context = g_load_prefab_context;') &&
      host.includes('g_load_prefab_context = g_mods_wait_context;') && host.includes('it.id = g_load_prefab_context;'),
      'a same-mode map change is published and worker readiness resumes the original click');

/* The host's side. */
check(host.indexOf('cmd == L"exportPrefab"') !== -1 && host.indexOf('ui_begin_pick(2)') !== -1, 'host: exportPrefab opens the Save dialog');
check(host.indexOf('cmd == L"importPrefab"') !== -1 && host.indexOf('ui_begin_pick(3)') !== -1, 'host: importPrefab opens the Open dialog');
check(host.indexOf('for_prefab_dialog(save)') !== -1, 'host: the prefab dialogs offer the prefab ZIP type');
check(host.indexOf('SH_MODULE_PREFAB_EXPORT') !== -1 && host.indexOf('SH_MODULE_PREFAB_IMPORT') !== -1, 'host: export and import ops');
const load = host.indexOf('static void ui_apply_load_prefab()');
check(load !== -1 && host.indexOf('if (!ui_resolve_prefab_mods(body)) return;', load) !== -1 &&
      host.indexOf('if (!ui_resolve_prefab_mods(body)) return;', load) < host.indexOf('ui_apply_place_module_prefab(body);', load) &&
      host.indexOf('if (!ui_resolve_prefab_mods(body)) return;', load) < host.indexOf('it.kind = SH_APPLY_PLACE_IN_MAP;', load),
      'host: the mods are resolved before either kind reaches the engine');
check(host.indexOf('SH_MODULE_PREFAB_MODS_RESOLVE') !== -1 && host.indexOf('SH_MODULE_PREFAB_MODS_POLL') !== -1,
      'host: resolve and poll');
check(host.indexOf('ui_poll_prefab_mods();') !== -1, 'host: the think loop follows a held placement');
check(host.indexOf('SH_MODULE_PREFAB_MODS_RECORD') !== -1 && host.indexOf('SH_MODULE_PREFAB_META_WRITE') !== -1,
      'host: Save records the mods and description writes keep them');
check(host.indexOf('L"},\\"mods\\":{"') !== -1 && host.indexOf('\\"modsStatus\\":\\"') !== -1, 'host: the list and details carry the status');

/* One file per prefab: the host reads details from the block, strips it before the engine, writes no sidecar. */
const strip = host.indexOf('body.resize(sh_prefab_block_strip(&body[0], body.size()));', load);
check(strip !== -1 && strip > host.indexOf('if (!ui_resolve_prefab_mods(body)) return;', load) &&
      strip < host.indexOf('ui_apply_place_module_prefab(body);', load) && strip < host.indexOf('it.kind = SH_APPLY_PLACE_IN_MAP;', load),
      'host: Load / Place strips the block after resolving the mods and before either kind reaches the engine');
const resolve = host.indexOf('static bool ui_resolve_prefab_mods(');
check(resolve !== -1 && host.indexOf('GetFileAttributesA(meta) == INVALID_FILE_ATTRIBUTES', resolve) === -1,
      'host: the mods are resolved whether or not a sidecar exists (the block holds them)');
const detail = host.indexOf('static void ui_send_prefab_detail(');
check(detail !== -1 && host.indexOf('sceneBody.resize(sh_prefab_block_strip(&sceneBody[0], sceneBody.size()));', detail) !== -1,
      'host: the selected prefab\'s scene ships without its block');
check(host.indexOf('call.op = SH_MODULE_PREFAB_META_READ;') !== -1 && host.indexOf('ui_prefab_view(path.c_str(), nullptr, nullptr, &fileType)') !== -1,
      'host: the list and details read the view from the file');
const rec = host.indexOf('call.op = SH_MODULE_PREFAB_MODS_RECORD;');
check(rec !== -1 && host.indexOf('call.path = path.c_str();', rec) !== -1 && host.indexOf('call.path = path.c_str();', rec) < rec + 200,
      'host: Create has the backend write the block into the prefab file');
const save = host.indexOf('static void ui_apply_save_prefab_meta(');
const saveEnd = host.indexOf('\n}\n', save);
check(save !== -1 && host.slice(save, saveEnd).indexOf('fopen_s') === -1 && host.slice(save, saveEnd).indexOf('DeleteFileA') === -1 &&
      host.slice(save, saveEnd).indexOf('call.path = path.c_str();') !== -1,
      'host: description and tag edits go to the file\'s block; no sidecar is written or deleted');
check(host.indexOf('call.meta_path = meta;\n    call.out = out;') === -1, 'host: the module prefab save writes no sidecar');

const folder = host.slice(host.indexOf('static void ui_apply_delete_folder()'), host.indexOf('/* Run enum_inherits'));
check(folder.includes('ui_prefab_can_move(dir + item + ".json"') &&
      folder.indexOf('ui_prefab_can_move(') < folder.indexOf('ui_move(') &&
      host.includes('if (!ui_prefab_can_move(path, reason)) return 0;') &&
      host.includes('if (!ui_prefab_can_move(oldp, &g_rename_reason))') &&
      host.includes('if (!ui_prefab_can_move(oldp, reason)) return 0;'),
      'moves, renames and recycling check the prefab before detaching its details');

/* The look. */
check(/\.pf-mods\s*\{/.test(css) && /\.pf-mods\.pf-mods-missing\s*\{/.test(css) && /\.pc-mods\s*\{/.test(css) &&
      /\.pc-mods-row\.missing \.pc-mods-state\s*\{/.test(css), 'styles for the pill and the section');

if (failures) { console.error('prefab_mods_test: ' + failures + ' failure(s)'); process.exit(1); }
console.log('prefab_mods_test: OK');
