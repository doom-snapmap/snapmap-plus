/* What the window tells the player when something fails: the cause in plain words and the next
 * step, never "see the log", a guess, or a console command. Logic lifted from the shipped page and
 * run; the last check scans every string the page and the host can show. Pure ASCII. */
'use strict';
const fs = require('fs');
const path = require('path');

const web = path.join(__dirname, '..', 'src', 'ui', 'webview');
const page = fs.readFileSync(path.join(web, 'mockup.html'), 'utf8').replace(/\r\n/g, '\n');
const host = fs.readFileSync(path.join(web, 'snapmap_plus_ui_webview.cpp'), 'utf8').replace(/\r\n/g, '\n');
let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }
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
new Function('api', fn('fsFailText') + fn('pfPlaceToast') + '\napi.fs = fsFailText; api.place = pfPlaceToast;')(api);

/* A refused file change names its cause, from the host's reason. */
check(/cannot be used as a folder name in Windows\. Choose another name\./.test(api.fs({result: -1}, '"CON"', 'folder name')),
      'a name Windows refuses says so');
check(/already there\. Choose another name\./.test(api.fs({result: 0, reason: 'exists'}, '"Doors"')), 'a taken name says so');
check(/open in another program\. Close it and try again\./.test(api.fs({result: 0, reason: 'inUse'}, '"Doors"')), 'a file in use says so');
check(/no longer there/.test(api.fs({result: 0, reason: 'missing'}, '"Doors"')), 'a missing file says so');
check(/Send feedback/.test(api.fs({result: 0, reason: ''}, '"Doors"')), 'another Windows refusal points at Send feedback');

/* Load / Place. */
check(/could not be read: the file is missing, damaged or in use\./.test(api.place({result: -1, name: 'Stairs'}).text), 'an unreadable prefab');
check(/busy\. Try Load \/ Place again/.test(api.place({result: 0, name: 'Stairs'}).text), 'a refused queue');
check(api.place({result: 1, name: 'Stairs', placed: 'queued'}).level === 'ok', 'a placed prefab');

/* A module prefab: the backend's sentence says which outcome happened; a short internal phrase never
 * reaches the player, the page's own sentence for the result does. */
const mod = {};
new Function('api', fn('pfModuleSentence') + fn('pfModulePlaceToast') + fn('pfModuleCreateToast') +
             '\napi.place = pfModulePlaceToast; api.create = pfModuleCreateToast;')(mod);
const undoStep = "This prefab put the map over the editor's limits and could not be taken back by itself; press Ctrl+Z to undo it.";
check(mod.place({result: -5, name: 'Hub', message: undoStep}).text === undoStep, 'over the limits, left as one undo step: Ctrl+Z');
check(mod.place({result: -5, name: 'Hub', message: undoStep}).level === 'warn', 'over the limits is a warning');
check(/does not fit/.test(mod.place({result: -5, name: 'Hub', message: ''}).text), 'over the limits without a sentence: the page says it');
check(mod.place({result: 0, name: 'Hub', message: 'Module prefabs are off until DOOM restarts, after an earlier failure.'}).text ===
      'Module prefabs are off until DOOM restarts, after an earlier failure.', 'module prefabs off until a restart says so');
check(/^The editor did not place "Hub"\./.test(mod.place({result: 0, name: 'Hub', message: 'the map changed before the placement; try again'}).text),
      'an internal phrase is not shown');
check(/Switch to Blueprint mode/.test(mod.place({result: -4, name: 'Hub', message: 'switch to Blueprint mode'}).text), 'Blueprint mode');
check(/could not be read/.test(mod.place({result: -1, name: 'Hub', message: 'not a module prefab'}).text), 'an unreadable file');
check(mod.place({result: -1, name: 'Hub', message: 'This prefab could not be placed: its file is damaged. Ask its author to export it again.'}).text
      .indexOf('Ask its author') > 0, 'a damaged file: the backend\'s sentence');
check(/needs rooms/.test(mod.place({result: -6, name: 'Hub', message: 'Hub needs rooms you do not have (DLC or a mod you do not have)'}).text),
      'missing rooms are named whatever the punctuation');
check(mod.place({result: 1, name: 'Hub'}).level === 'ok', 'placed');
check(mod.create({result: -1, name: 'Hub', message: 'Select the modules again and press Create.'}).text === 'Select the modules again and press Create.',
      'a save refused: the backend\'s sentence');
check(/Send feedback/.test(mod.create({result: -1, name: 'Hub', message: 'the prefab file could not be written'}).text), 'a save failed');
check(/already exists/.test(mod.create({result: 0, name: 'Hub'}).text), 'a taken name');
check(host.includes('L",\\"message\\":\\""; m += sh_webview_json::escape_wide(g_create_module_message.c_str());'),
      'the host passes a refused save\'s sentence');

/* No string the player can see sends them to a log, guesses, or names a console command. */
const shown = (page.match(/'[^'\n]*'/g) || []).concat(host.match(/L"[^"\n]*"/g) || []);
const banned = [/\(see (the )?log\)/i, /see the log/i, /editor down\?/i, /most likely\)/i, /\bRun sh_/, /`sh /,
                /This build's backend/, /engine hook/i, /reverse-engineered/i, / evidence, /];
for (const text of shown) for (const b of banned) check(!b.test(text), 'shown text: ' + text);

if (failures) { console.error(failures + ' message check(s) failed'); process.exit(1); }
console.log('page_messages_test: all checks passed');
