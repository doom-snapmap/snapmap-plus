/* The Prefabs tab's mods: the status a prefab's snapmapPlus block records (backend/prefab_mods.c),
   the list pill, the Prefab Details rows, the Load / Place answers about mods and the Export and
   Import buttons, lifted from the shipped HTML; and the host's side of each message: the block
   read for the list and details, stripped before the engine, and no sidecar written. Pure ASCII. */
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

const door = {id: 'test.door', version: '1.0.0', bundle: 'a'.repeat(64), size: 100, installed: true};
const ghost = {id: 'ghost.mod', version: '2.0', bundle: '9'.repeat(64), size: 1048576, installed: false};
const both = JSON.stringify({count: 2, missing: 1, mods: [door, ghost]});

/* Parsing: the host's escaped string or an object; anything malformed is no mods. */
let st = api.parse(both);
check(st.count === 2 && st.missing === 1 && st.mods[1].id === 'ghost.mod', 'status string parses: ' + JSON.stringify(st));
check(api.parse({mods: [door]}).count === 1 && api.parse({mods: [door]}).missing === 0, 'an object parses too');
check(api.parse('').count === 0 && api.parse('{not json').count === 0 && api.parse(null).count === 0, 'malformed reads as none');
check(api.parse({mods: [{id: ''}, null, {version: '1'}]}).count === 0, 'rows without an id are dropped');
check(api.parse({mods: [{id: 'x'}]}).missing === 1, 'a row without installed:true counts as missing');
check(api.label(door) === 'test.door 1.0.0' && api.label({id: 'x'}) === 'x', 'labels are "id version"');

/* The list pill. */
check(api.badge(api.parse('')) === null, 'no mods, no pill');
let b = api.badge(st);
check(b && b.text === 'Needs mods: ghost.mod' && /pf-mods-missing/.test(b.cls), 'a missing mod names it: ' + JSON.stringify(b));
check(b && b.title === 'test.door 1.0.0 (installed)\nghost.mod 2.0 (not installed)', 'the title lists every mod and its state');
b = api.badge(api.parse({mods: [door]}));
check(b && b.text === 'Mods: test.door' && b.cls === 'pf-mods', 'installed mods are listed: ' + JSON.stringify(b));
b = api.badge(api.parse({mods: [door, Object.assign({}, door, {id: 'other.mod'})]}));
check(b && b.text === 'Mods: test.door +1', 'more mods are counted');
b = api.badge(api.parse({mods: [ghost, Object.assign({}, ghost, {id: 'g2'}), door]}));
check(b && b.text === 'Needs mods: ghost.mod +1', 'only the missing ones are named when any is missing');

/* Prefab Details. */
let d = api.detail(st);
check(d.rows.length === 2 && d.rows[0].label === 'test.door 1.0.0' && d.rows[0].state === 'Installed' && !d.rows[0].missing &&
      d.rows[1].state === 'Not installed' && d.rows[1].missing, 'one row per mod with its state');
check(d.note === 'Needs mods: 1 is not installed. Load / Place offers to download them when the mod store has them; ' +
      'otherwise ask the prefab\'s author for its ZIP export.', 'the note says what Load / Place does: ' + d.note);
check(api.detail(api.parse({mods: [ghost, ghost]})).note.indexOf('Needs mods: 2 are not installed.') === 0, 'plural note');
check(api.detail(api.parse({mods: [door]})).note === 'Every mod this prefab needs is installed.', 'all installed');
check(api.detail(api.parse('')).rows.length === 0 && api.detail(api.parse('')).note === '', 'no mods, no section');

/* Load / Place answers about mods. */
let t = api.loadToast({result: 3, name: 'Door', message: 'This prefab needs 1 mod you do not have; checking the mod store.'});
check(t && t.level === 'ok' && t.text === 'This prefab needs 1 mod you do not have; checking the mod store.', 'held: the message');
t = api.loadToast({result: -7, name: 'Door', message: ''});
check(t && t.level === 'warn' && t.text === '"Door" was not placed: its mods are not installed', 'refused without text');
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
check(src.indexOf('pfShowMods(pfModsParse(d.modsStatus))') !== -1, 'details render the mods status');
check(src.indexOf('var mbadge = pfModsBadge(prefabMods[') !== -1, 'the list row shows the pill');
check(src.indexOf('var dmods = d.mods && typeof d.mods') !== -1 && src.indexOf("['mods', 'needs mods']") !== -1,
      'the list reads the mods map and the filter finds them');
const lp = src.indexOf("d.kind === 'loadPrefabResult'");
check(lp !== -1 && src.indexOf('var pml = pfModsLoadToast(d);', lp) !== -1 &&
      src.indexOf('var pml = pfModsLoadToast(d);', lp) < src.indexOf("d.prefabKind === 'modules'", lp),
      'mods answers are told before the kind-specific toasts');
check(src.indexOf("d.kind === 'prefabFileResult'") !== -1, 'Export and Import answers are handled');
/* P29's warning stays. */
check(src.indexOf("pfShowModuleCheck(selectedPrefabKind === 'modules' ? d.moduleCheck : null)") !== -1 && /id="pcNeeds"/.test(src),
      'the module check warning is kept');

/* The host's side. */
check(host.indexOf('cmd == L"exportPrefab"') !== -1 && host.indexOf('poc_begin_pick(2)') !== -1, 'host: exportPrefab opens the Save dialog');
check(host.indexOf('cmd == L"importPrefab"') !== -1 && host.indexOf('poc_begin_pick(3)') !== -1, 'host: importPrefab opens the Open dialog');
check(host.indexOf('for_prefab_dialog(save)') !== -1, 'host: the prefab dialogs offer the prefab ZIP type');
check(host.indexOf('SH_MODULE_PREFAB_EXPORT') !== -1 && host.indexOf('SH_MODULE_PREFAB_IMPORT') !== -1, 'host: export and import ops');
const load = host.indexOf('static void poc_apply_load_prefab()');
check(load !== -1 && host.indexOf('if (!poc_resolve_prefab_mods(body)) return;', load) !== -1 &&
      host.indexOf('if (!poc_resolve_prefab_mods(body)) return;', load) < host.indexOf('poc_apply_place_module_prefab(body);', load) &&
      host.indexOf('if (!poc_resolve_prefab_mods(body)) return;', load) < host.indexOf('it.kind = 2;', load),
      'host: the mods are resolved before either kind reaches the engine');
check(host.indexOf('SH_MODULE_PREFAB_MODS_RESOLVE') !== -1 && host.indexOf('SH_MODULE_PREFAB_MODS_POLL') !== -1,
      'host: resolve and poll');
check(host.indexOf('poc_poll_prefab_mods();') !== -1, 'host: the think loop follows a held placement');
check(host.indexOf('SH_MODULE_PREFAB_MODS_RECORD') !== -1 && host.indexOf('SH_MODULE_PREFAB_META_WRITE') !== -1,
      'host: Save records the mods and description writes keep them');
check(host.indexOf('L"},\\"mods\\":{"') !== -1 && host.indexOf('\\"modsStatus\\":\\"') !== -1, 'host: the list and details carry the status');

/* One file per prefab: the host reads details from the block, strips it before the engine, writes no sidecar. */
const strip = host.indexOf('body.resize(sh_prefab_block_strip(&body[0], body.size()));', load);
check(strip !== -1 && strip > host.indexOf('if (!poc_resolve_prefab_mods(body)) return;', load) &&
      strip < host.indexOf('poc_apply_place_module_prefab(body);', load) && strip < host.indexOf('it.kind = 2;', load),
      'host: Load / Place strips the block after resolving the mods and before either kind reaches the engine');
const resolve = host.indexOf('static bool poc_resolve_prefab_mods(');
check(resolve !== -1 && host.indexOf('GetFileAttributesA(meta) == INVALID_FILE_ATTRIBUTES', resolve) === -1,
      'host: the mods are resolved whether or not a sidecar exists (the block holds them)');
const detail = host.indexOf('static void poc_send_prefab_detail(');
check(detail !== -1 && host.indexOf('sceneBody.resize(sh_prefab_block_strip(&sceneBody[0], sceneBody.size()));', detail) !== -1,
      'host: the selected prefab\'s scene ships without its block');
check(host.indexOf('call.op = SH_MODULE_PREFAB_META_READ;') !== -1 && host.indexOf('poc_prefab_view(path, mp, nullptr)') !== -1,
      'host: the list and details read the view from the file (else its sidecar)');
const rec = host.indexOf('call.op = SH_MODULE_PREFAB_MODS_RECORD;');
check(rec !== -1 && host.indexOf('call.path = path;', rec) !== -1 && host.indexOf('call.path = path;', rec) < rec + 200,
      'host: Create has the backend write the block into the prefab file');
const save = host.indexOf('static void poc_apply_save_prefab_meta(');
const saveEnd = host.indexOf('\n}\n', save);
check(save !== -1 && host.slice(save, saveEnd).indexOf('fopen_s') === -1 && host.slice(save, saveEnd).indexOf('DeleteFileA') === -1 &&
      host.slice(save, saveEnd).indexOf('call.path = path;') !== -1,
      'host: description and tag edits go to the file\'s block; no sidecar is written or deleted');
check(host.indexOf('call.meta_path = meta;\n    call.out = out;') === -1, 'host: the module prefab save writes no sidecar');

/* The look. */
check(/\.pf-mods\s*\{/.test(css) && /\.pf-mods\.pf-mods-missing\s*\{/.test(css) && /\.pc-mods\s*\{/.test(css) &&
      /\.pc-mods-row\.missing \.pc-mods-state\s*\{/.test(css), 'styles for the pill and the section');

if (failures) { console.error('prefab_mods_test: ' + failures + ' failure(s)'); process.exit(1); }
console.log('prefab_mods_test: OK');
