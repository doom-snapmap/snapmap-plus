/* publishing_key_page_driver.js -- run by tests/publishing_key_dispatch_test.cpp: the Settings page's publishing key
 * (src/ui/webview/publishing_key.js) on a stand-in DOM, driven over stdin one command per line: MSG <json> (a host
 * message), SETTINGS, NOTNOW, BACKUP <passphrase>, RESTORE <passphrase>, STATE, QUIT. It answers on stdout with POST
 * <json> per message the page posts, STATE <json>, and END when the command is done. Timers run at once. Pure ASCII. */
'use strict';
const path = require('path');
const readline = require('readline');
const pk = require(path.join(__dirname, '..', 'src', 'ui', 'webview', 'publishing_key.js'));

const ids = {};
function el(id) {
  const e = {id: id, style: {}, value: '', textContent: '', disabled: false, attrs: {}, handlers: {}};
  e.setAttribute = function (n, v) { e.attrs[n] = String(v); };
  e.addEventListener = function (ev, fn) { (e.handlers[ev] = e.handlers[ev] || []).push(fn); };
  e.focus = function () {};
  e.fire = function (ev) { (e.handlers[ev] || []).forEach(function (fn) { fn({preventDefault: function () {}}); }); };
  ids[id] = e;
  return e;
}
['pubKeyModal', 'pubKeyTitle', 'pubKeyNote', 'pubKeyPassRow', 'pubKeyPass2Row', 'pubKeyPass', 'pubKeyPass2',
 'pubKeyError', 'pubKeyCancel', 'pubKeyOk', 'menuKeyBackup', 'menuKeyRestore', 'menuKeyNote'].forEach(el);
const settings = el('settingsLabel');
const out = function (s) { process.stdout.write(s + '\n'); };
const timers = [];
const ui = pk.create({byId: function (id) { return ids[id] || null; },
                      post: function (m) { out('POST ' + JSON.stringify(m)); },
                      toast: function (t, kind) { out('TOAST ' + JSON.stringify({text: t, kind: kind})); },
                      closeMenus: function () {}, settingsLabel: settings,
                      after: function (fn) { timers.push(fn); }});
ui.bind();
out('END');

const rl = readline.createInterface({input: process.stdin});
rl.on('line', function (line) {
  const sp = line.indexOf(' '), cmd = sp < 0 ? line : line.slice(0, sp), rest = sp < 0 ? '' : line.slice(sp + 1);
  if (cmd === 'MSG') ui.onMessage(JSON.parse(rest));
  else if (cmd === 'SETTINGS') settings.fire('click');
  else if (cmd === 'NOTNOW') ids.pubKeyCancel.fire('click');
  else if (cmd === 'BACKUP' || cmd === 'RESTORE') {
    ids[cmd === 'BACKUP' ? 'menuKeyBackup' : 'menuKeyRestore'].fire('click');
    ids.pubKeyPass.value = rest;
    ids.pubKeyPass2.value = rest;
    ids.pubKeyOk.fire('click');
  } else if (cmd === 'STATE') out('STATE ' + JSON.stringify(ui.state()));
  else if (cmd === 'QUIT') { rl.close(); return; }
  while (timers.length) timers.shift()();
  out('END');
});
