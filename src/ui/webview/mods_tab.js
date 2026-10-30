/* The Mods tab: the installed mods, what the open map needs, and the few actions a player uses
   (install a folder, open the mods folder, remove a mod, migrate the old overrides folder).
   Browser global + CommonJS test export. Pure ASCII.

   The backend (backend/modui.c, the `mods` interface slot) builds one document on the game thread:
   one row per mod folder in mods\ (its key is the folder name; two folders declaring one id are both
   rows, marked duplicateId; a package a map delivered is a row like any other), whether overrides\
   still holds entries to migrate, and the open map. The host posts it as {kind:'mods', data} after
   every action; showing the tab asks for a rescan (which also builds every mod folder whose files
   or mod.json changed), so the folder is reread whenever the player looks. Nothing polls. Diagnostics (every
   bundle's state, conflicts, shadows, verification) are the console's `sh_mods doctor`, not this
   tab's. The list is a plain list: a store holds tens of mods, a few hundred at most, and the page
   scrolls it natively (wheel, scrollbar, keyboard). All text goes through textContent. */
var modsTab = (function () {
  var STATES = {
    installed: {label: 'Installed', tone: 'ok'},
    attention: {label: 'Needs attention', tone: 'warn'}
  };
  var DUPLICATE_TEXT = 'another installed mod declares this id; change one id in its mod.json';
  var ROW_STATES = {
    active:     {label: 'Installed', tone: 'ok', detail: 'Installed and active on this machine.'},
    superseded: {label: 'Installed', tone: 'ok', detail: 'Installed; a newer build of the mod serves its files.'},
    unindexed:  {label: 'Not scanned', tone: 'warn', detail: 'Installed but not scanned yet; reopen the Mods tab, then reload the map.'},
    skipped:    {label: 'Needs attention', tone: 'err', detail: 'Installed but not in use.'},
    missing:    {label: 'Missing', tone: 'err', detail: 'Not installed. Entities that need it will not load.'}
  };
  var ACTIONS = ['install', 'remove', 'migrate', 'rescan', 'openFolder'];

  function str(v) { return typeof v === 'string' ? v : (v === null || v === undefined ? '' : String(v)); }
  function num(v) { var n = Number(v); return isFinite(n) ? n : 0; }
  function arr(v) { return Array.isArray(v) ? v : []; }
  function bool(v) { return v === true; }

  function normMod(b) {
    b = b && typeof b === 'object' ? b : {};
    var state = Object.prototype.hasOwnProperty.call(STATES, b.state) ? b.state : 'attention';
    return {key: str(b.key), id: str(b.id), name: str(b.name), author: str(b.author), version: str(b.version),
            build: num(b.build), size: num(b.size), files: num(b.files), pointers: num(b.pointers), state: state,
            cause: str(b.cause), otherBuilds: num(b.otherBuilds), duplicateId: bool(b.duplicateId),
            editorRows: num(b.editorRows), folder: str(b.folder)};
  }
  function normNames(v) { return arr(v).map(str).filter(function (s) { return s.length > 0; }); }
  function normRow(r) {
    r = r && typeof r === 'object' ? r : {};
    var state = Object.prototype.hasOwnProperty.call(ROW_STATES, r.state) ? r.state : 'missing';
    var names = normNames(r.names);
    return {id: str(r.id), version: str(r.version), bundle: str(r.bundle), size: num(r.size), state: state,
            cause: str(r.cause), key: str(r.key), otherKey: str(r.otherKey), names: names,
            nameCount: Math.max(num(r.nameCount), names.length)};
  }
  function normPlaced(p) {
    p = p && typeof p === 'object' ? p : {};
    var names = normNames(p.names);
    return {id: str(p.id), version: str(p.version), bundle: str(p.bundle), size: num(p.size), key: str(p.key),
            viaNeeds: bool(p.viaNeeds), inManifest: bool(p.inManifest), names: names,
            nameCount: Math.max(num(p.nameCount), names.length)};
  }
  function title(b) { return b.name || b.id || b.key; }

  /* The host's payload as the tab uses it. error is set for an unusable payload. */
  function normalize(payload) {
    if (!payload || typeof payload !== 'object' || Array.isArray(payload))
      return {ok: false, error: 'The mods list is unavailable.', mods: [], rows: [], placed: [], entities: -2, names: 0,
              root: '', gen: 0, migrate: {entries: 0, pending: 0, running: false}};
    var map = payload.map && typeof payload.map === 'object' ? payload.map : {};
    var mg = payload.migrate && typeof payload.migrate === 'object' ? payload.migrate : {};
    var mods = arr(payload.mods).map(normMod);
    mods.sort(function (a, b) {
      var x = title(a).toLowerCase(), y = title(b).toLowerCase();
      return x === y ? (a.key < b.key ? -1 : a.key > b.key ? 1 : 0) : (x < y ? -1 : 1);
    });
    return {ok: true, error: payload.storeReadable === false ? 'The mods folder cannot be read.' : '',
            root: str(payload.root), gen: num(payload.gen), mods: mods,
            migrate: {entries: num(mg.entries), pending: num(mg.pending), running: bool(mg.running)},
            rows: arr(map.rows).map(normRow), placed: arr(map.placed).map(normPlaced),
            entities: map.entities === undefined ? -2 : num(map.entities), names: num(map.names)};
  }

  function formatSize(bytes) {
    var n = num(bytes);
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
    return (n / 1073741824).toFixed(1) + ' GB';
  }
  function plural(n, one, many) { return n + ' ' + (n === 1 ? one : (many || one + 's')); }
  /* "12 files" or "12 files, 9 from the installed game" (stock pointers: nothing stored for them). */
  function filesText(b) {
    var p = num(b.pointers);
    return plural(b.files, 'file') + (p > 0 ? ', ' + p + ' from the installed game' : '');
  }
  /* id@version, the name a map records; the folder for a mod that was never built. */
  function idText(b) { return b.id ? b.id + '@' + (b.version || '0.0.0') : b.key; }

  /* The label, tone and detail line of a row. */
  function stateInfo(b) {
    var s = STATES[b.state] || STATES.attention, parts = [];
    if (b.duplicateId) parts.push(DUPLICATE_TEXT);
    else if (b.state === 'attention') parts.push(b.cause || 'not in use');
    if (b.otherBuilds > 0) parts.push(plural(b.otherBuilds, 'other build') + ' installed');
    return {label: b.duplicateId ? STATES.attention.label : s.label, tone: b.duplicateId ? STATES.attention.tone : s.tone,
            detail: parts.join('; ')};
  }
  function rowInfo(r) {
    var s = ROW_STATES[r.state] || ROW_STATES.missing, detail = s.detail;
    if (r.state === 'missing' && r.otherKey) detail = 'Not installed; another build (' + r.otherKey + ') is, but the map names this one.';
    if (r.state === 'skipped' && r.cause) detail = 'Installed but not in use: ' + r.cause;
    return {label: s.label, tone: s.tone, detail: detail};
  }

  function filterMods(mods, query) {
    var q = str(query).trim().toLowerCase();
    if (!q) return mods.slice();
    return mods.filter(function (b) { return (b.name + '\n' + b.id + '\n' + b.key).toLowerCase().indexOf(q) !== -1; });
  }

  /* What saving the open map would record, compared with what it carried when it loaded. */
  function mapSummary(m) {
    if (m.entities === -1) return {text: 'Open a map in the editor to see the mods it needs.', tone: ''};
    if (m.entities < 0) return {text: 'This build cannot read the placed entities.', tone: ''};
    var missing = m.rows.filter(function (r) { return r.state === 'missing' || r.state === 'skipped'; }).length;
    var added = m.placed.filter(function (p) { return !p.inManifest; }).length;
    var placedBundles = {};
    m.placed.forEach(function (p) { placedBundles[p.bundle] = true; });
    var dropped = m.rows.filter(function (r) { return !placedBundles[r.bundle]; }).length;
    var parts = [];
    if (!m.placed.length && !m.rows.length) return {text: 'This map uses no mods. Saving it records none.', tone: 'ok'};
    parts.push('Saving now records ' + plural(m.placed.length, 'mod') + '.');
    if (added) parts.push(plural(added, 'mod') + ' new since the map loaded.');
    if (dropped) parts.push(plural(dropped, 'recorded mod is', 'recorded mods are') + ' no longer placed and will be dropped.');
    if (missing) parts.push(plural(missing, 'recorded mod is', 'recorded mods are') + ' not available on this machine.');
    return {text: parts.join(' '), tone: missing ? 'err' : added || dropped ? 'warn' : 'ok'};
  }

  function actionMessage(action, arg) {
    if (ACTIONS.indexOf(action) < 0) return null;
    var m = {cmd: 'modsAction', action: action};
    if (arg !== undefined && arg !== null && arg !== '') m.arg = str(arg);
    return m;
  }

  function removeConfirmText(b) {
    var lines = ['Remove ' + title(b) + ' (' + idText(b) + ')?', ''];
    if (b.otherBuilds > 0) lines.push('Its ' + plural(b.otherBuilds, 'other installed build') + ' ' + (b.otherBuilds === 1 ? 'goes' : 'go') + ' too.');
    lines.push('Its files are deleted from the mods folder; a folder you made there yourself is moved to removed-mods instead. ' +
               'Maps that use it will miss its entities until it is installed again.');
    return lines.join('\n');
  }

  /* ------------------------------------------------------------------------------------------ DOM */

  /* env: {document, byId(id), post(message), toast(text, kind), confirm(text, onOk)} */
  function create(env) {
    var doc = env.document, byId = env.byId;
    var model = normalize(null), loaded = false, waiting = false, busy = '', quiet = false, selectedKey = '';
    var view = [], filter = '', lastError = '';

    function el(tag, cls, text) {
      var n = doc.createElement(tag);
      if (cls) n.className = cls;
      if (text !== undefined && text !== null) n.textContent = str(text);
      return n;
    }
    function clear(n) { while (n && n.firstChild) n.removeChild(n.firstChild); }
    function pill(text, tone) { return el('span', 'mods-pill' + (tone ? ' ' + tone : ''), text); }

    function setStatus(text, tone) {
      var s = byId('modsStatus');
      if (!s) return;
      s.textContent = str(text);
      s.className = 'mods-status' + (tone ? ' ' + tone : '');
    }
    var BUSY_TEXT = {install: 'Installing...', remove: 'Removing...', rescan: 'Reading the mods folder...',
                     migrate: 'Migrating the old overrides folder; this can take a minute...'};

    function selectedMod() {
      for (var i = 0; i < model.mods.length; i++) if (model.mods[i].key === selectedKey) return model.mods[i];
      return null;
    }

    function renderButtons() {
      ['modsInstallBtn', 'modsRemoveBtn', 'modsMigrateBtn', 'modsRefreshBtn'].forEach(function (id) {
        var b = byId(id);
        if (b) b.disabled = !!busy || (id === 'modsRemoveBtn' && !selectedMod());
      });
      var mg = byId('modsMigrateBtn');
      if (mg) {
        var show = loaded && model.migrate.pending > 0 && !model.migrate.running;
        mg.style.display = show || busy === 'migrate' ? '' : 'none';
        mg.title = 'Move ' + plural(model.migrate.pending, 'entry', 'entries') +
                   ' of the old overrides folder into the mods folder';
      }
    }
    function setBusy(action) {
      busy = action || '';
      renderButtons();
      if (busy && !(busy === 'rescan' && quiet)) setStatus(BUSY_TEXT[busy] || 'Working...', 'busy');
    }

    /* ---- the installed list (plain: every row is mounted, the browser scrolls it) */
    function renderList() {
      var box = byId('modsList');
      if (!box) return;
      clear(box);
      if (!loaded) { box.appendChild(el('div', 'list-empty', waiting ? 'Reading installed mods...' : 'Open this tab to list installed mods.')); return; }
      if (lastError) { box.appendChild(el('div', 'list-empty mods-error', lastError)); return; }
      if (!model.mods.length) {
        box.appendChild(el('div', 'list-empty', 'No mods installed. Use the install button (the download icon) to add one, or put a mod folder in the mods folder.'));
        return;
      }
      if (!view.length) { box.appendChild(el('div', 'list-empty', 'No installed mod matches the filter.')); return; }
      for (var i = 0; i < view.length; i++) box.appendChild(modRow(view[i]));
    }
    function modRow(b) {
      var info = stateInfo(b), on = b.key === selectedKey;
      var row = el('div', 'mods-row' + (on ? ' selected' : ''));
      row.setAttribute('role', 'option');
      row.setAttribute('data-key', b.key);
      row.setAttribute('aria-selected', on ? 'true' : 'false');
      var top = el('div', 'mods-row-top');
      top.appendChild(el('span', 'mods-row-name', title(b)));
      top.appendChild(pill(info.label, info.tone));
      row.appendChild(top);
      row.appendChild(el('div', 'mods-row-sub', idText(b) + ' \u00b7 ' + formatSize(b.size) + ' \u00b7 ' + filesText(b)));
      if (info.detail) row.appendChild(el('div', 'mods-row-detail' + (b.state === 'attention' || b.duplicateId ? ' warn' : ''), info.detail));
      row.addEventListener('click', function () { select(b.key); });
      return row;
    }

    function select(key) {
      selectedKey = key;
      var box = byId('modsList');
      if (box) {
        /* Restyle in place: rebuilding the rows would lose nothing, but this keeps the scroll anchor. */
        var rows = box.children || [];
        for (var i = 0; i < rows.length; i++) {
          var r = rows[i], on = r.getAttribute && r.getAttribute('data-key') === key;
          if (!r.getAttribute || r.getAttribute('data-key') === null) continue;
          r.className = 'mods-row' + (on ? ' selected' : '');
          r.setAttribute('aria-selected', on ? 'true' : 'false');
          if (on && r.scrollIntoView) r.scrollIntoView({block: 'nearest'});
        }
      }
      renderDetail();
      renderButtons();
    }
    /* Arrow keys, Home and End move the selection; Page Up and Page Down scroll natively. */
    function onListKey(e) {
      var keys = {ArrowDown: 1, ArrowUp: -1, Home: -Infinity, End: Infinity};
      if (!view.length || !Object.prototype.hasOwnProperty.call(keys, e.key)) return;
      var at = -1;
      for (var i = 0; i < view.length; i++) if (view[i].key === selectedKey) at = i;
      var step = keys[e.key];
      var next = step === -Infinity ? 0 : step === Infinity ? view.length - 1 : at < 0 ? 0 : Math.max(0, Math.min(view.length - 1, at + step));
      if (e.preventDefault) e.preventDefault();
      select(view[next].key);
    }

    /* ---- the selected mod */
    function field(box, label, value) {
      var r = el('div', 'mods-field');
      r.appendChild(el('span', 'mods-field-label', label));
      r.appendChild(el('span', 'mods-field-value', value));
      box.appendChild(r);
    }
    function renderDetail() {
      var box = byId('modsDetail');
      if (!box) return;
      clear(box);
      var b = selectedMod();
      if (!b) { box.appendChild(el('div', 'mods-note', loaded && model.mods.length ? 'Select a mod to see its details.' : '')); return; }
      var info = stateInfo(b);
      var head = el('div', 'mods-detail-head');
      head.appendChild(el('span', 'mods-detail-name', title(b)));
      head.appendChild(pill(info.label, info.tone));
      box.appendChild(head);
      if (info.detail) box.appendChild(el('div', 'mods-note' + (b.state === 'attention' || b.duplicateId ? ' warn' : ''), info.detail));
      field(box, 'Mod', idText(b));
      if (b.author) field(box, 'Author', b.author);
      field(box, 'Size', formatSize(b.size) + ', ' + filesText(b));
      if (b.editorRows) field(box, 'Editor rows', String(b.editorRows));
      field(box, 'Folder', b.folder || b.key);
    }

    /* ---- the open map */
    function namesText(names, count) {
      if (!count) return '';
      var shown = names.slice(0, 3).join(', ');
      return shown + (count > 3 ? ' and ' + (count - 3) + ' more' : '');
    }
    function renderMap() {
      var box = byId('modsMap');
      if (!box) return;
      clear(box);
      if (!loaded) return;
      var s = mapSummary(model);
      box.appendChild(el('div', 'mods-note' + (s.tone ? ' ' + s.tone : ''), s.text));
      if (model.entities < 0) return;
      if (model.rows.length) {
        box.appendChild(el('div', 'mods-sub', 'Recorded when the map loaded'));
        model.rows.forEach(function (r) {
          var info = rowInfo(r), line = el('div', 'mods-map-row');
          var top = el('div', 'mods-row-top');
          top.appendChild(el('span', 'mods-row-name', r.id + ' ' + r.version));
          top.appendChild(pill(info.label, info.tone));
          line.appendChild(top);
          line.appendChild(el('div', 'mods-row-sub', info.detail + (r.nameCount ? ' Names: ' + namesText(r.names, r.nameCount) + '.' : '')));
          box.appendChild(line);
        });
      }
      if (model.placed.length) {
        box.appendChild(el('div', 'mods-sub', 'Placed now (recorded at save)'));
        model.placed.forEach(function (p) {
          var line = el('div', 'mods-map-row'), top = el('div', 'mods-row-top');
          top.appendChild(el('span', 'mods-row-name', p.id + ' ' + p.version));
          if (!p.inManifest) top.appendChild(pill('New', 'warn'));
          if (p.viaNeeds) top.appendChild(pill('Needed by another mod', ''));
          line.appendChild(top);
          line.appendChild(el('div', 'mods-row-sub', (p.key || p.bundle.slice(0, 16)) + ' \u00b7 ' + formatSize(p.size) +
            (p.nameCount ? ' \u00b7 ' + namesText(p.names, p.nameCount) : '')));
          box.appendChild(line);
        });
      }
    }

    function renderHead() {
      var badge = byId('modsBadge');
      if (badge) badge.textContent = String(model.mods.length);
      var rootLine = byId('modsRoot');
      if (rootLine) { rootLine.textContent = model.root; rootLine.title = model.root; }
    }

    function renderAll() {
      view = filterMods(model.mods, filter);
      if (selectedKey && !selectedMod()) selectedKey = '';
      renderHead();
      renderList();
      renderDetail();
      renderMap();
      renderButtons();
    }

    /* ---- requests */
    function act(action, arg) {
      if (busy) { env.toast('Another mods action is still running.', 'warn'); return false; }
      var m = actionMessage(action, arg);
      if (!m) return false;
      quiet = false;
      if (action !== 'openFolder') setBusy(action);
      env.post(m);
      return true;
    }
    /* Reread the folder: a rescan, which the host follows with a fresh document. Quiet: its result
       only shows when it fails. A running action already ends with a fresh document. */
    function refresh() {
      if (busy || waiting) return false;
      waiting = true;
      quiet = true;
      setBusy('rescan');
      env.post(actionMessage('rescan'));
      if (!loaded) renderList();
      return true;
    }

    /* ---- messages */
    function attentionText() {
      var n = model.mods.filter(function (b) { return b.state === 'attention' || b.duplicateId; }).length;
      return n ? plural(n, 'mod needs', 'mods need') + ' attention.' : '';
    }
    function onMods(d) {
      waiting = false;
      loaded = true;
      if (!d || d.data === null || d.data === undefined) {
        lastError = d && d.error ? str(d.error) : 'The mods list is unavailable.';
        model = normalize(null);
      } else {
        model = normalize(d.data);
        lastError = model.ok ? model.error : 'The mods list is unavailable.';
        if (!busy) setStatus(attentionText(), '');
      }
      renderAll();
    }
    function onResult(d) {
      var action = str(d.action), data = d.data && typeof d.data === 'object' ? d.data : {};
      var message = str(d.message || data.message), ok = d.ok === true || data.ok === true;
      var wasQuiet = quiet && action === 'rescan';
      quiet = false;
      setBusy('');
      if (action === 'install' && data.cancelled) { setStatus('', ''); return; }
      if (wasQuiet) {
        /* A failed rescan is followed by a document unless the host cannot reach the backend at all. */
        waiting = false;
        if (!ok && !loaded) { loaded = true; lastError = message || 'The mods list is unavailable.'; renderAll(); }
        if (!ok) setStatus(message, 'err');
        return;
      }
      setStatus(message, ok ? 'ok' : 'err');
      if (message) env.toast(message, ok ? 'ok' : 'err');
      if (action === 'install' && ok && data.key) selectedKey = str(data.key);
      if (action === 'remove' && ok) selectedKey = '';
    }

    /* ---- wiring */
    function bind() {
      var on = function (id, ev, fn) { var n = byId(id); if (n) n.addEventListener(ev, fn); };
      on('modsInstallBtn', 'click', function () { act('install'); });
      on('modsFolderBtn', 'click', function () { act('openFolder'); });
      on('modsMigrateBtn', 'click', function () { act('migrate'); });
      on('modsRemoveBtn', 'click', function () {
        var b = selectedMod();
        if (!b || busy) return;
        env.confirm(removeConfirmText(b), function () { act('remove', b.key); });
      });
      on('modsRefreshBtn', 'click', function () { refresh(); });
      on('modsFilter', 'input', function () { filter = str(byId('modsFilter').value); view = filterMods(model.mods, filter); renderList(); });
      on('modsList', 'keydown', onListKey);
      renderButtons();
    }

    return {
      bind: bind,
      /* The tab became visible: reread the folder (explicit demand, never a timer). */
      show: function () { refresh(); },
      refresh: refresh,
      act: act,
      select: select,
      onMessage: function (d) {
        if (!d || typeof d !== 'object') return false;
        if (d.kind === 'mods') { onMods(d); return true; }
        if (d.kind === 'modsActionResult') { onResult(d); return true; }
        return false;
      },
      state: function () { return {model: model, loaded: loaded, waiting: waiting, busy: busy, selectedKey: selectedKey, view: view, error: lastError}; }
    };
  }

  return {STATES: STATES, ROW_STATES: ROW_STATES, ACTIONS: ACTIONS, DUPLICATE_TEXT: DUPLICATE_TEXT, normalize: normalize,
          formatSize: formatSize, stateInfo: stateInfo, rowInfo: rowInfo, filesText: filesText, idText: idText,
          filterMods: filterMods, mapSummary: mapSummary, actionMessage: actionMessage, removeConfirmText: removeConfirmText,
          create: create};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = modsTab;
