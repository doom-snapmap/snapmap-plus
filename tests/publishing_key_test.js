/* Settings' publishing key (publishing_key.js): the offer after the first publish (shown once, Not now answers it),
 * a backup's passphrase typed twice and checked before anything is sent, a restore's once, the backend's sentences
 * shown in the dialog, the fields cleared when it closes, and the Mods tab's results left alone. Run against a small
 * stand-in DOM. Pure ASCII. */
'use strict';
const path = require('path');
const pk = require(path.join(__dirname, '..', 'src', 'ui', 'webview', 'publishing_key.js'));

let failures = 0;
function check(ok, message) { if (!ok) { console.error('[FAIL] ' + message); failures++; } }

const ids = {};
function el(id) {
  const e = {id: id, style: {}, value: '', textContent: '', disabled: false, attrs: {}, handlers: {}};
  e.setAttribute = function (n, v) { e.attrs[n] = String(v); };
  e.addEventListener = function (ev, fn) { (e.handlers[ev] = e.handlers[ev] || []).push(fn); };
  e.focus = function () {};
  e.fire = function (ev, arg) { (e.handlers[ev] || []).forEach(function (fn) { fn(arg || {preventDefault: function () {}}); }); };
  ids[id] = e;
  return e;
}
['pubKeyModal', 'pubKeyTitle', 'pubKeyNote', 'pubKeyPassRow', 'pubKeyPass2Row', 'pubKeyPass', 'pubKeyPass2',
 'pubKeyError', 'pubKeyCancel', 'pubKeyOk', 'menuKeyBackup', 'menuKeyRestore', 'menuKeyNote'].forEach(el);
const label = el('settingsLabel');

const posts = [], toasts = [];
const ui = pk.create({byId: function (id) { return ids[id] || null; }, post: function (m) { posts.push(m); },
                      toast: function (t, k) { toasts.push([t, k]); }, closeMenus: function () {}, settingsLabel: label});
ui.bind();
check(posts.length === 1 && posts[0].action === 'publishingKey' && posts[0].arg === 'status', 'the key state is asked at start');
label.fire('click');
check(posts.length === 2 && posts[1].arg === 'status', 'and again when the Settings menu opens');

/* a mods document is read for the key, not taken from the Mods tab */
check(ui.onMessage({kind: 'mods', data: {publishingKey: {exists: false, id: '', offer: false}}}) === false, 'a mods document passes on');
check(ids.menuKeyNote.textContent === 'No publishing key yet', 'the menu says there is no key');
check(ui.onMessage({kind: 'modsActionResult', action: 'mapUpdate', data: {}}) === false, 'other results pass on');

/* a backup with no key: said, nothing opens */
ids.menuKeyBackup.fire('click');
check(!ui.state().open && toasts.length === 1 && toasts[0][0] === pk.NO_KEY_TEXT, 'no key: the backup is not offered');

/* the first publish made the key: the offer opens once */
ui.onMessage({kind: 'mods', data: {publishingKey: {exists: true, id: '1a2b3c4d5e6f7081', offer: true}}});
check(ui.state().open && ui.state().page === 'offer', 'the offer opens');
check(ids.pubKeyNote.textContent === pk.OFFER_TEXT && ids.pubKeyOk.textContent === 'Save backup' &&
      ids.pubKeyCancel.textContent === 'Not now', 'the offer says SPEC 4.2 with Save backup / Not now');
check(ids.menuKeyNote.textContent === 'Publishing key 1a2b3c4d', 'the menu names the key');
ids.pubKeyCancel.fire('click');
check(!ui.state().open && posts[posts.length - 1].arg === 'later', 'Not now answers the offer');
ui.onMessage({kind: 'mods', data: {publishingKey: {exists: true, id: '1a2b3c4d5e6f7081', offer: true}}});
check(!ui.state().open, 'the offer is shown once');

/* the backup: the two passphrases are checked before anything goes */
const before = posts.length;
ids.menuKeyBackup.fire('click');
check(ui.state().open && ui.state().page === 'backup' && ids.pubKeyPass2Row.style.display === '', 'the backup asks twice');
ids.pubKeyPass.value = 'short'; ids.pubKeyPass2.value = 'short';
ids.pubKeyOk.fire('click');
check(ids.pubKeyError.textContent === pk.SHORT_TEXT && posts.length === before, 'too short: refused here');
ids.pubKeyPass.value = 'correct horse battery'; ids.pubKeyPass2.value = 'correct horse batterx';
ids.pubKeyOk.fire('click');
check(ids.pubKeyError.textContent === pk.MISMATCH_TEXT && posts.length === before, 'not the same: refused here');
ids.pubKeyPass2.value = 'correct horse battery';
ids.pubKeyOk.fire('click');
check(posts.length === before + 1 && posts[before].arg === 'backup\tcorrect horse battery' && ui.state().busy,
      'the backup goes with the passphrase; the host picks the file');
/* the picker was closed: back to the dialog */
ui.onMessage({kind: 'modsActionResult', action: 'publishingKey', ok: false, data: {cancelled: true}});
check(ui.state().open && !ui.state().busy, 'a closed picker keeps the dialog');
ids.pubKeyOk.fire('click');
ui.onMessage({kind: 'modsActionResult', action: 'publishingKey',
              data: {ok: true, message: 'Your publishing key is saved in k.snapmap-plus-key.', key: {exists: true, id: '1a2b3c4d5e6f7081', offer: false}}});
check(!ui.state().open && toasts[toasts.length - 1][0].indexOf('saved in') > 0, 'saved: closed and said');
check(ids.pubKeyPass.value === '' && ids.pubKeyPass2.value === '', 'the passphrase is not kept');

/* the restore: one passphrase, the backend's refusal shown in the dialog */
ids.menuKeyRestore.fire('click');
check(ui.state().page === 'restore' && ids.pubKeyPass2Row.style.display === 'none', 'the restore asks once');
ids.pubKeyPass.value = 'wrong passphrase';
ids.pubKeyOk.fire('click');
check(posts[posts.length - 1].arg === 'restore\twrong passphrase', 'the restore goes');
ui.onMessage({kind: 'modsActionResult', action: 'publishingKey',
              data: {ok: false, message: 'That passphrase does not open this backup.', key: {exists: true, id: '1a2b3c4d5e6f7081'}}});
check(ui.state().open && ids.pubKeyError.textContent === 'That passphrase does not open this backup.', 'refused: said in the dialog');
ui.cancel();
check(!ui.state().open && ids.pubKeyPass.value === '', 'Cancel closes and clears');

check(pk.checkPassphrase('restore', 'longenough', '') === '', 'a restore needs no second passphrase');

/* Not now while another mods action runs: the host turns it away (no data), so it is sent again until the backend's
 * answer shows the offer answered; then it stops */
{
  const timers = [], sent = [];
  const ids2 = {};
  const mk = function (id) { const e = el(id); ids2[id] = e; return e; };
  ['pubKeyModal', 'pubKeyTitle', 'pubKeyNote', 'pubKeyPassRow', 'pubKeyPass2Row', 'pubKeyPass', 'pubKeyPass2',
   'pubKeyError', 'pubKeyCancel', 'pubKeyOk', 'menuKeyBackup', 'menuKeyRestore', 'menuKeyNote'].forEach(mk);
  const ui2 = pk.create({byId: function (id) { return ids2[id] || null; }, post: function (m) { sent.push(m); },
                         after: function (fn) { timers.push(fn); }});
  ui2.bind();
  ui2.onMessage({kind: 'mods', data: {publishingKey: {exists: true, id: '1a2b3c4d5e6f7081', offer: true}}});
  ids2.pubKeyCancel.fire('click');
  const laters = function () { return sent.filter(function (m) { return m.arg === 'later'; }).length; };
  check(laters() === 1 && ui2.state().laterPending, 'Not now is sent');
  ui2.onMessage({kind: 'modsActionResult', action: 'publishingKey', ok: false,
                 message: 'Another mods action is still running.', data: null});
  check(timers.length === 1, 'a turned-away Not now is sent again after a pause');
  timers.shift()();
  check(laters() === 2, 'and is sent again');
  ui2.onMessage({kind: 'modsActionResult', action: 'publishingKey',
                 data: {ok: true, message: '', key: {exists: true, id: '1a2b3c4d5e6f7081', offer: false}}});
  check(!ui2.state().laterPending && timers.length === 0, 'the backend answered: no more sends');
}

/* every request names the host's route as the window reads it ("cmd", mods_message.h): a message routed any other
 * way is dropped by the host without a word (R-LB1: Not now never reached the backend) */
check(posts.length > 0 && posts.every(function (m) { return m.cmd === 'modsAction' && !('kind' in m); }),
      'every request is routed by cmd: modsAction');

if (failures) { console.error('publishing_key_test: ' + failures + ' failure(s)'); process.exit(1); }
console.log('publishing_key_test: all checks passed');
