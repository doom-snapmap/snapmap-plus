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

/* No string the player can see sends them to a log, guesses, or names a console command. */
const shown = (page.match(/'[^'\n]*'/g) || []).concat(host.match(/L"[^"\n]*"/g) || []);
const banned = [/\(see (the )?log\)/i, /see the log/i, /editor down\?/i, /most likely\)/i, /\bRun sh_/, /`sh /,
                /This build's backend/, /engine hook/i, /reverse-engineered/i, / evidence, /];
for (const text of shown) for (const b of banned) check(!b.test(text), 'shown text: ' + text);

if (failures) { console.error(failures + ' message check(s) failed'); process.exit(1); }
console.log('page_messages_test: all checks passed');
