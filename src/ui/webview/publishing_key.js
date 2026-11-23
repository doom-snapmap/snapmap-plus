/* publishing_key.js -- Settings: the publishing key (SPEC-distribution 4.2), and its backup offer after the first
 * publish. Backend: the Mods slot's SH_MODS_ACT_KEY ("publishingKey", modclaim.h modclaim_key_action); the native
 * host asks for the file in its own picker, so the page sends only the operation and the passphrase. Never in the
 * Mods tab (2.10). The passphrase is never kept: the fields are cleared when the dialog closes. Node-testable. */
var publishingKey = (function () {
  'use strict';

  var OFFER_TEXT = 'Snapmap+ made a publishing key for you. It lets only you republish or unpublish your maps\' mod ' +
                   'content. Save a backup of it?';
  var BACKUP_TITLE = 'Save a backup of your publishing key';
  var BACKUP_NOTE = 'Choose a passphrase for the backup. You need it to restore the key, and Snapmap+ cannot recover ' +
                    'it. Next, choose where to save the file.';
  var RESTORE_TITLE = 'Restore your publishing key';
  var RESTORE_NOTE = 'Type the backup\'s passphrase. Next, choose the backup file.';
  var MISMATCH_TEXT = 'The two passphrases are not the same.';
  var SHORT_TEXT = 'A backup\'s passphrase needs at least 8 characters.';
  var NO_KEY_TEXT = 'You have no publishing key yet. Snapmap+ makes one the first time you publish a map with mods.';
  var MIN_LENGTH = 8;

  /* The menu note: which key publishes, or that there is none yet. */
  function keyNote(key) {
    if (!key || !key.exists) return 'No publishing key yet';
    return 'Publishing key ' + String(key.id || '').slice(0, 8);
  }

  /* What a passphrase pair says before anything is sent: '' when it may go. */
  function checkPassphrase(page, a, b) {
    a = String(a || '');
    if (a.length < MIN_LENGTH) return SHORT_TEXT;
    if (page === 'backup' && a !== String(b || '')) return MISMATCH_TEXT;
    return '';
  }

  function create(env) {
    var byId = env.byId, post = env.post, toast = env.toast || function () {};
    var st = {open: false, page: '', busy: false, error: '', key: null, offered: false};

    function show(id, on) { var e = byId(id); if (e) e.style.display = on ? '' : 'none'; }
    function setText(id, t) { var e = byId(id); if (e) e.textContent = t; }
    function clearFields() {
      ['pubKeyPass', 'pubKeyPass2'].forEach(function (id) { var e = byId(id); if (e) e.value = ''; });
    }

    function render() {
      var ov = byId('pubKeyModal');
      if (!ov) return;
      ov.style.display = st.open ? 'flex' : 'none';
      ov.setAttribute('aria-hidden', st.open ? 'false' : 'true');
      if (!st.open) return;
      var offer = st.page === 'offer', backup = st.page === 'backup';
      setText('pubKeyTitle', offer ? 'Your publishing key' : backup ? BACKUP_TITLE : RESTORE_TITLE);
      setText('pubKeyNote', offer ? OFFER_TEXT : backup ? BACKUP_NOTE : RESTORE_NOTE);
      show('pubKeyPassRow', !offer);
      show('pubKeyPass2Row', backup);
      setText('pubKeyError', st.error);
      show('pubKeyError', !!st.error);
      setText('pubKeyCancel', offer ? 'Not now' : 'Cancel');
      setText('pubKeyOk', offer ? 'Save backup' : backup ? 'Save backup...' : 'Choose backup...');
      ['pubKeyOk', 'pubKeyCancel', 'pubKeyPass', 'pubKeyPass2'].forEach(function (id) {
        var e = byId(id); if (e) e.disabled = st.busy;
      });
    }

    function open(page) {
      st.open = true; st.page = page; st.error = ''; st.busy = false;
      clearFields();
      render();
      var f = byId(page === 'offer' ? 'pubKeyOk' : 'pubKeyPass');
      if (f && f.focus) f.focus();
    }

    function close() {
      st.open = false; st.busy = false; st.error = '';
      clearFields();
      render();
    }

    function cancel() {
      if (st.busy) return;
      if (st.page === 'offer') post({kind: 'modsAction', action: 'publishingKey', arg: 'later'});
      close();
    }

    function confirm() {
      if (st.busy) return;
      if (st.page === 'offer') { open('backup'); return; }
      var a = (byId('pubKeyPass') || {}).value, b = (byId('pubKeyPass2') || {}).value;
      var bad = checkPassphrase(st.page, a, b);
      if (bad) { st.error = bad; render(); return; }
      st.busy = true; st.error = '';
      render();
      post({kind: 'modsAction', action: 'publishingKey', arg: st.page + '\t' + a});
    }

    function openBackup() {
      if (st.key && !st.key.exists) { toast(NO_KEY_TEXT, 'warn'); return; }
      open('backup');
    }

    function noteKey(key) {
      if (!key || typeof key !== 'object') return;
      st.key = key;
      setText('menuKeyNote', keyNote(key));
      if (key.offer && !st.offered && !st.open) { st.offered = true; open('offer'); }
    }

    function onResult(d) {
      var data = d.data || {};
      if (data.key) noteKey(data.key);
      if (data.cancelled) { st.busy = false; render(); return; }   /* the file picker was closed */
      if (!st.open || (st.page !== 'backup' && st.page !== 'restore')) return;
      st.busy = false;
      if (data.ok) { close(); toast(String(data.message || ''), 'ok'); return; }
      st.error = String(data.message || d.message || 'Snapmap+ could not do that.');
      render();
    }

    return {
      bind: function () {
        var on = function (id, ev, fn) { var e = byId(id); if (e) e.addEventListener(ev, fn); };
        // The key's state: asked once now, and each time the Settings menu opens (it also arms the pushed list,
        // whose next document carries the offer after a first publish).
        post({kind: 'modsAction', action: 'publishingKey', arg: 'status'});
        if (env.settingsLabel) env.settingsLabel.addEventListener('click', function () {
          post({kind: 'modsAction', action: 'publishingKey', arg: 'status'});
        });
        on('menuKeyBackup', 'click', function () { if (env.closeMenus) env.closeMenus(); openBackup(); });
        on('menuKeyRestore', 'click', function () { if (env.closeMenus) env.closeMenus(); open('restore'); });
        on('pubKeyCancel', 'click', cancel);
        on('pubKeyOk', 'click', confirm);
        on('pubKeyModal', 'keydown', function (e) {
          if (e.key === 'Escape') { e.preventDefault(); cancel(); }
          else if (e.key === 'Enter') { e.preventDefault(); confirm(); }
        });
      },
      /* true when the message was this page's (a publishingKey result); a mods document is read, not taken */
      onMessage: function (d) {
        if (!d || typeof d !== 'object') return false;
        if (d.kind === 'mods' && d.data && d.data.publishingKey) { noteKey(d.data.publishingKey); return false; }
        if (d.kind === 'modsActionResult' && d.action === 'publishingKey') { onResult(d); return true; }
        return false;
      },
      openBackup: openBackup,
      openRestore: function () { open('restore'); },
      confirm: confirm,
      cancel: cancel,
      state: function () { return {open: st.open, page: st.page, busy: st.busy, error: st.error, key: st.key, offered: st.offered}; }
    };
  }

  return {create: create, keyNote: keyNote, checkPassphrase: checkPassphrase, OFFER_TEXT: OFFER_TEXT,
          MISMATCH_TEXT: MISMATCH_TEXT, SHORT_TEXT: SHORT_TEXT, NO_KEY_TEXT: NO_KEY_TEXT};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = publishingKey;
