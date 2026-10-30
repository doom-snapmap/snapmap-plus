/* The Mods tab: installed mod bundles, what the open map needs, shadows and conflicts, and the
   sh_mods actions. Browser global + CommonJS test export. Pure ASCII.

   The backend (backend/modui.c, the `mods` interface slot) builds one document on the game thread;
   the host posts it as {kind:'mods', data} when the page asks (modsList) and again after every
   action. Nothing here polls. The bundle list and the shadow list mount only the rows in view
   (fixed row heights, a sizer for the scroll range); the shadow list is not built at all until its
   section is opened. All text goes through textContent. */
var modsTab = (function () {
  var ROW_HEIGHT = 40, SHADOW_ROW_HEIGHT = 38, OVERSCAN = 6;

  var STATES = {
    active:     {label: 'Active', tone: 'ok', detail: 'Installed and serving its files.'},
    legacy:     {label: 'Legacy', tone: 'ok', detail: 'A converted first-generation package, serving its files.'},
    superseded: {label: 'Superseded', tone: 'warn', detail: 'A newer build of this mod serves its files.'},
    unindexed:  {label: 'Not scanned', tone: 'warn', detail: 'Installed after the last scan; Rescan to use it.'},
    skipped:    {label: 'Skipped', tone: 'err', detail: 'Not served.'}
  };
  var ROW_STATES = {
    active:     {label: 'Installed', tone: 'ok', detail: 'Installed and active on this machine.'},
    superseded: {label: 'Installed', tone: 'ok', detail: 'Installed; a newer build of the mod serves its files.'},
    unindexed:  {label: 'Not scanned', tone: 'warn', detail: 'Installed but not scanned yet; Rescan, then reload the map.'},
    skipped:    {label: 'Skipped', tone: 'err', detail: 'Installed but skipped.'},
    missing:    {label: 'Missing', tone: 'err', detail: 'Not installed. Entities that need it will not load.'}
  };
  var SHADOW_KINDS = {
    stock:    {label: 'Shadow', tone: 'warn', text: 'Applies on this machine only.'},
    legacy:   {label: 'Legacy shadow', tone: 'warn', text: 'Changes what maps mean.'},
    conflict: {label: 'Conflict', tone: 'err', text: 'Two mods supply this name; neither is served.'}
  };
  var ACTIONS = ['install', 'verify', 'rescan', 'palette', 'pruneList', 'pruneApply', 'openFolder'];

  function str(v) { return typeof v === 'string' ? v : (v === null || v === undefined ? '' : String(v)); }
  function num(v) { var n = Number(v); return isFinite(n) ? n : 0; }
  function arr(v) { return Array.isArray(v) ? v : []; }
  function bool(v) { return v === true; }

  function normBundle(b) {
    b = b && typeof b === 'object' ? b : {};
    var state = Object.prototype.hasOwnProperty.call(STATES, b.state) ? b.state : 'skipped';
    return {key: str(b.key), id: str(b.id), name: str(b.name), author: str(b.author), version: str(b.version),
            build: num(b.build), hash: str(b.hash), size: num(b.size), files: num(b.files), pointers: num(b.pointers), state: state,
            cause: str(b.cause), legacy: bool(b.legacy), verified: bool(b.verified), installedAt: num(b.installedAt),
            editorRows: num(b.editorRows), shadows: num(b.shadows), supersededPaths: num(b.supersededPaths),
            supersededBy: str(b.supersededBy), folder: str(b.folder)};
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
  function normShadow(s) {
    s = s && typeof s === 'object' ? s : {};
    var kind = Object.prototype.hasOwnProperty.call(SHADOW_KINDS, s.kind) ? s.kind : 'stock';
    return {request: str(s.request), kind: kind, shadow: bool(s.shadow), served: str(s.served), installed: bool(s.installed),
            bundles: arr(s.bundles).map(str), resolution: str(s.resolution), reason: str(s.reason)};
  }

  /* The host's payload as the tab uses it. error is set for an unusable payload. */
  function normalize(payload) {
    if (!payload || typeof payload !== 'object' || Array.isArray(payload))
      return {ok: false, error: 'The mods list is unavailable.', bundles: [], rows: [], placed: [], shadows: [],
              totals: {stock: 0, legacy: 0, conflict: 0, total: 0, sent: 0}, entities: -2, names: 0, root: '', gen: 0, index: {}};
    var map = payload.map && typeof payload.map === 'object' ? payload.map : {};
    var t = payload.shadowTotals && typeof payload.shadowTotals === 'object' ? payload.shadowTotals : {};
    var shadows = arr(payload.shadows).map(normShadow);
    var bundles = arr(payload.bundles).map(normBundle);
    bundles.sort(function (a, b) {
      var x = (a.name || a.id || a.key).toLowerCase(), y = (b.name || b.id || b.key).toLowerCase();
      if (x !== y) return x < y ? -1 : 1;
      return b.build - a.build;
    });
    return {ok: true, error: payload.storeReadable === false ? 'The mods folder cannot be read.' : '',
            root: str(payload.root), gen: num(payload.gen), index: payload.index && typeof payload.index === 'object' ? payload.index : {},
            bundles: bundles, rows: arr(map.rows).map(normRow), placed: arr(map.placed).map(normPlaced),
            entities: map.entities === undefined ? -2 : num(map.entities), names: num(map.names), shadows: shadows,
            totals: {stock: num(t.stock), legacy: num(t.legacy), conflict: num(t.conflict),
                     total: Math.max(num(t.total), shadows.length), sent: Math.max(num(t.sent), shadows.length)}};
  }

  function formatSize(bytes) {
    var n = num(bytes);
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
    return (n / 1073741824).toFixed(1) + ' GB';
  }
  function plural(n, one, many) { return n + ' ' + (n === 1 ? one : (many || one + 's')); }
  function versionText(b) { return (b.version || '0.0.0') + (b.build ? ' build ' + b.build : ''); }
  /* "12 files" or "12 files, 9 from the installed game" (stock pointers: nothing stored for them). */
  function filesText(b) {
    var p = num(b.pointers);
    return plural(b.files, 'file') + (p > 0 ? ', ' + p + ' from the installed game' : '');
  }
  function title(b) { return b.name || b.id || b.key; }

  /* The label, tone and one-line explanation of a bundle's state. */
  function stateInfo(b) {
    var s = STATES[b.state] || STATES.skipped, detail = s.detail;
    if (b.state === 'superseded' && b.supersededBy)
      detail = 'A newer build (' + b.supersededBy + ') serves ' + (b.supersededPaths === 1 ? '1 of its files' : b.supersededPaths + ' of its files') + '.';
    if (b.state === 'skipped' && b.cause) detail = 'Not served: ' + b.cause;
    if ((b.state === 'active' || b.state === 'legacy') && b.cause) detail = s.detail + ' Warning: ' + b.cause;
    return {label: s.label, tone: s.tone, detail: detail};
  }
  function rowInfo(r) {
    var s = ROW_STATES[r.state] || ROW_STATES.missing, detail = s.detail;
    if (r.state === 'missing' && r.otherKey) detail = 'Not installed; another build (' + r.otherKey + ') is, but the map names this one.';
    if (r.state === 'skipped' && r.cause) detail = 'Installed but skipped: ' + r.cause;
    return {label: s.label, tone: s.tone, detail: detail};
  }
  /* A conflict says which copy serves and why (the backend's resolution). */
  var CONFLICT_TEXT = {
    composed:  'Several mods add entries to this list; the entries of all of them are served.',
    withdrawn: 'Several mods supply this name; the copy chosen names a model nothing provides, so the game copy is used.'
  };
  function shadowInfo(s) {
    var info = SHADOW_KINDS[s.kind] || SHADOW_KINDS.stock;
    /* A stock pointer serves the path: the installed game's bytes. */
    if (s.kind !== 'conflict' && s.installed)
      return {label: info.label, tone: info.tone, text: info.text + " Served from the installed game's own copy."};
    if (s.kind !== 'conflict') return info;
    if (CONFLICT_TEXT[s.resolution]) return {label: info.label, tone: 'warn', text: CONFLICT_TEXT[s.resolution]};
    if (s.resolution === 'chosen')
      return {label: info.label, tone: 'warn', text: 'Several mods supply this name; ' + (s.served || 'one of them') + ' serves it' +
        (s.reason ? ' (' + s.reason + ')' : '') + '.'};
    return info;
  }

  function filterBundles(bundles, query) {
    var q = str(query).trim().toLowerCase();
    if (!q) return bundles.slice();
    return bundles.filter(function (b) {
      return (b.name + '\n' + b.id + '\n' + b.key + '\n' + b.author + '\n' + b.state).toLowerCase().indexOf(q) !== -1;
    });
  }

  /* Rows [start, end) to mount for a scroll position; never more than the viewport plus overscan. */
  function windowRange(scrollTop, viewport, rowHeight, count, overscan) {
    var h = rowHeight > 0 ? rowHeight : ROW_HEIGHT, o = overscan === undefined ? OVERSCAN : overscan;
    var top = Math.max(0, num(scrollTop)), vh = Math.max(0, num(viewport));
    var start = Math.min(Math.max(0, count), Math.max(0, Math.floor(top / h) - o));
    var end = Math.min(count, Math.ceil((top + (vh || h * 20)) / h) + o);
    if (end < start) end = start;
    return {start: start, end: end};
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

  function pruneConfirmText(candidates) {
    var lines = ['Remove ' + plural(candidates.length, 'build') + ' that a newer build of the same mod covers?', ''];
    candidates.slice(0, 12).forEach(function (c) {
      lines.push(str(c.key) + ' (' + formatSize(c.size) + '), covered by ' + str(c.coveredBy));
    });
    if (candidates.length > 12) lines.push('...and ' + (candidates.length - 12) + ' more');
    lines.push('', 'Maps that name a removed build get the newer build\'s files. This cannot be undone.');
    return lines.join('\n');
  }

  /* ------------------------------------------------------------------------------------------ DOM */

  /* env: {document, byId(id), post(message), toast(text, kind), confirm(text, onOk), raf(fn)} */
  function create(env) {
    var doc = env.document, byId = env.byId;
    var model = normalize(null), loaded = false, waiting = false, busy = '', selectedKey = '';
    var view = [], filter = '', shadowsOpen = false, lastError = '';
    var listQueued = false, shadowQueued = false;
    var raf = env.raf || function (fn) { fn(); };

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

    function setBusy(action) {
      busy = action || '';
      ['modsInstallBtn', 'modsRescanBtn', 'modsVerifyBtn', 'modsPaletteBtn', 'modsPruneBtn', 'modsVerifyOneBtn'].forEach(function (id) {
        var b = byId(id);
        if (b) b.disabled = !!busy || (id === 'modsVerifyOneBtn' && !selectedBundle());
      });
      if (busy) setStatus(BUSY_TEXT[busy] || 'Working...', 'busy');
    }
    var BUSY_TEXT = {install: 'Installing...', verify: 'Verifying files...', rescan: 'Rereading the mods folder...',
                     palette: 'Rebuilding the editor palette...', pruneList: 'Looking for removable builds...',
                     pruneApply: 'Removing builds...', list: 'Reading installed mods...'};

    function selectedBundle() {
      for (var i = 0; i < model.bundles.length; i++) if (model.bundles[i].key === selectedKey) return model.bundles[i];
      return null;
    }

    /* ---- bundle list (windowed) */
    function renderList() {
      listQueued = false;
      var box = byId('modsList');
      if (!box) return;
      clear(box);
      if (!loaded) { box.appendChild(el('div', 'list-empty', waiting ? 'Reading installed mods...' : 'Open this tab to list installed mods.')); return; }
      if (lastError) { box.appendChild(el('div', 'list-empty mods-error', lastError)); return; }
      if (!model.bundles.length) {
        box.appendChild(el('div', 'list-empty', 'No mods installed. Use Install to add a mod folder, or put one in the mods folder and Rescan.'));
        return;
      }
      if (!view.length) { box.appendChild(el('div', 'list-empty', 'No installed mod matches the filter.')); return; }
      /* A shorter list (filter, refresh) leaves the old scroll position past its end. */
      var maxTop = Math.max(0, view.length * ROW_HEIGHT - box.clientHeight);
      if (box.scrollTop > maxTop) box.scrollTop = maxTop;
      var range = windowRange(box.scrollTop, box.clientHeight, ROW_HEIGHT, view.length, OVERSCAN);
      var sizer = el('div', 'mods-sizer');
      sizer.style.height = (view.length * ROW_HEIGHT) + 'px';
      box.appendChild(sizer);
      for (var i = range.start; i < range.end; i++) sizer.appendChild(bundleRow(view[i], i));
    }
    function bundleRow(b, i) {
      var info = stateInfo(b);
      var row = el('div', 'mods-row' + (b.key === selectedKey ? ' selected' : ''));
      row.style.top = (i * ROW_HEIGHT) + 'px';
      row.setAttribute('role', 'option');
      row.setAttribute('data-key', b.key);
      row.setAttribute('aria-selected', b.key === selectedKey ? 'true' : 'false');
      var top = el('div', 'mods-row-top');
      top.appendChild(el('span', 'mods-row-name', title(b)));
      top.appendChild(pill(info.label, info.tone));
      if (b.legacy && b.state !== 'legacy') top.appendChild(pill('Legacy', ''));
      var sub = el('div', 'mods-row-sub', b.key + ' \u00b7 ' + formatSize(b.size) + ' \u00b7 ' + filesText(b));
      row.title = info.detail;
      row.appendChild(top);
      row.appendChild(sub);
      row.addEventListener('click', function () { select(b.key); });
      return row;
    }
    function queueList() { if (!listQueued) { listQueued = true; raf(renderList); } }

    function select(key) {
      selectedKey = key;
      queueList();
      renderDetail();
      setBusy(busy);
    }

    /* ---- selected bundle */
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
      var b = selectedBundle();
      if (!b) { box.appendChild(el('div', 'mods-note', loaded && model.bundles.length ? 'Select a mod to see its details.' : '')); return; }
      var info = stateInfo(b);
      var head = el('div', 'mods-detail-head');
      head.appendChild(el('span', 'mods-detail-name', title(b)));
      head.appendChild(pill(info.label, info.tone));
      box.appendChild(head);
      box.appendChild(el('div', 'mods-note' + (info.tone === 'err' ? ' err' : ''), info.detail));
      field(box, 'Mod', b.id || '(unbuilt)');
      field(box, 'Version', versionText(b));
      if (b.author) field(box, 'Author', b.author);
      field(box, 'Size', formatSize(b.size) + ', ' + filesText(b));
      field(box, 'Editor rows', String(b.editorRows));
      if (b.shadows) field(box, 'Shadows', plural(b.shadows, 'stock file'));
      field(box, 'Verified', b.verified ? 'Yes' : 'No');
      if (b.installedAt) field(box, 'Installed', new Date(b.installedAt * 1000).toLocaleString());
      if (b.hash) field(box, 'Bundle', b.hash.slice(0, 16));
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

    /* ---- shadows and conflicts (built only when opened, windowed) */
    function renderShadowSummary() {
      var s = byId('modsShadowsSummary');
      if (!s) return;
      var t = model.totals;
      s.textContent = 'Shadows and conflicts' + (loaded ? ' (' + plural(t.stock, 'shadow') + ', ' + plural(t.legacy, 'legacy shadow') +
        ', ' + plural(t.conflict, 'conflict') + ')' : '');
    }
    function renderShadows() {
      shadowQueued = false;
      var box = byId('modsShadowsBody');
      if (!box) return;
      clear(box);
      if (!shadowsOpen || !loaded) return;
      if (!model.shadows.length) { box.appendChild(el('div', 'mods-note', 'No installed mod replaces a game file or another mod\'s file.')); return; }
      var range = windowRange(box.scrollTop, box.clientHeight, SHADOW_ROW_HEIGHT, model.shadows.length, OVERSCAN);
      var sizer = el('div', 'mods-sizer');
      sizer.style.height = (model.shadows.length * SHADOW_ROW_HEIGHT) + 'px';
      box.appendChild(sizer);
      for (var i = range.start; i < range.end; i++) {
        var sh = model.shadows[i], info = shadowInfo(sh);
        var row = el('div', 'mods-shadow-row');
        row.style.top = (i * SHADOW_ROW_HEIGHT) + 'px';
        var top = el('div', 'mods-row-top');
        top.appendChild(pill(info.label, info.tone));
        top.appendChild(el('span', 'mods-shadow-path', sh.request));
        row.appendChild(top);
        row.appendChild(el('div', 'mods-row-sub', info.text + ' ' + (sh.kind === 'conflict' ? 'From ' : 'By ') + sh.bundles.join(', ') + '.'));
        row.title = sh.request;
        sizer.appendChild(row);
      }
      if (model.totals.total > model.shadows.length) {
        var more = el('div', 'mods-note', (model.totals.total - model.shadows.length) + ' more not listed.');
        more.style.marginTop = (model.shadows.length * SHADOW_ROW_HEIGHT) + 'px';
        box.appendChild(more);
      }
    }
    function queueShadows() { if (!shadowQueued) { shadowQueued = true; raf(renderShadows); } }

    function renderHead() {
      var badge = byId('modsBadge');
      if (badge) badge.textContent = String(model.bundles.length);
      var rootLine = byId('modsRoot');
      if (rootLine) { rootLine.textContent = model.root; rootLine.title = model.root; }
    }

    function renderAll() {
      view = filterBundles(model.bundles, filter);
      if (selectedKey && !selectedBundle()) selectedKey = '';
      renderHead();
      renderList();
      renderDetail();
      renderMap();
      renderShadowSummary();
      renderShadows();
      setBusy(busy);
    }

    /* ---- requests */
    function refresh() {
      if (waiting) return false;
      waiting = true;
      env.post({cmd: 'modsList'});
      if (!loaded) renderList();
      return true;
    }
    function act(action, arg) {
      if (busy) { env.toast('Another mods action is still running.', 'warn'); return false; }
      var m = actionMessage(action, arg);
      if (!m) return false;
      if (action !== 'openFolder') setBusy(action);
      env.post(m);
      return true;
    }

    /* ---- messages */
    function onMods(d) {
      waiting = false;
      loaded = true;
      if (!d || d.data === null || d.data === undefined) {
        lastError = d && d.error ? str(d.error) : 'The mods list is unavailable.';
        model = normalize(null);
      } else {
        model = normalize(d.data);
        lastError = model.ok ? model.error : 'The mods list is unavailable.';
        if (!busy) setStatus(model.index && model.index.admitted !== undefined
          ? plural(num(model.index.admitted), 'mod') + ' serving, ' + plural(num(model.index.files), 'file') + '.' : '', '');
      }
      renderAll();
    }
    function onResult(d) {
      var action = str(d.action), data = d.data && typeof d.data === 'object' ? d.data : {};
      var message = str(d.message || data.message), ok = d.ok === true || data.ok === true;
      setBusy('');
      if (action === 'pruneList' && ok) {
        var cands = arr(data.candidates);
        if (!cands.length) { env.toast(message || 'No build can be removed.', ''); setStatus(message, ''); return; }
        setStatus(message, '');
        env.confirm(pruneConfirmText(cands), function () {
          act('pruneApply', cands.map(function (c) { return str(c.key); }).join('\n'));
        });
        return;
      }
      if (action === 'install' && data.cancelled) { setStatus('', ''); return; }
      setStatus(message, ok ? 'ok' : 'err');
      if (message) env.toast(message, ok ? 'ok' : (d.busy ? 'warn' : 'err'));
      if (action === 'install' && ok && data.key) selectedKey = str(data.key);
    }

    /* ---- wiring */
    function bind() {
      var on = function (id, ev, fn) { var n = byId(id); if (n) n.addEventListener(ev, fn); };
      on('modsInstallBtn', 'click', function () { act('install'); });
      on('modsRescanBtn', 'click', function () { act('rescan'); });
      on('modsVerifyBtn', 'click', function () { act('verify'); });
      on('modsVerifyOneBtn', 'click', function () { var b = selectedBundle(); if (b) act('verify', b.key); });
      on('modsPaletteBtn', 'click', function () { act('palette'); });
      on('modsPruneBtn', 'click', function () { act('pruneList'); });
      on('modsFolderBtn', 'click', function () { act('openFolder'); });
      on('modsRefreshBtn', 'click', function () { refresh(); });
      on('modsFilter', 'input', function () { filter = str(byId('modsFilter').value); view = filterBundles(model.bundles, filter); byId('modsList').scrollTop = 0; renderList(); });
      on('modsList', 'scroll', queueList);
      on('modsShadowsBody', 'scroll', queueShadows);
      on('modsShadows', 'toggle', function () { shadowsOpen = !!byId('modsShadows').open; renderShadows(); });
      setBusy('');
    }

    return {
      bind: bind,
      /* The tab became visible: read the current state (explicit demand, never a timer). */
      show: function () { refresh(); queueList(); },
      refresh: refresh,
      act: act,
      select: select,
      onMessage: function (d) {
        if (!d || typeof d !== 'object') return false;
        if (d.kind === 'mods') { onMods(d); return true; }
        if (d.kind === 'modsActionResult') { onResult(d); return true; }
        return false;
      },
      state: function () { return {model: model, loaded: loaded, waiting: waiting, busy: busy, selectedKey: selectedKey, view: view, shadowsOpen: shadowsOpen, error: lastError}; }
    };
  }

  return {ROW_HEIGHT: ROW_HEIGHT, SHADOW_ROW_HEIGHT: SHADOW_ROW_HEIGHT, OVERSCAN: OVERSCAN, STATES: STATES, ROW_STATES: ROW_STATES,
          SHADOW_KINDS: SHADOW_KINDS, ACTIONS: ACTIONS, normalize: normalize, formatSize: formatSize, stateInfo: stateInfo,
          rowInfo: rowInfo, shadowInfo: shadowInfo, filesText: filesText, filterBundles: filterBundles, windowRange: windowRange, mapSummary: mapSummary,
          actionMessage: actionMessage, pruneConfirmText: pruneConfirmText, create: create};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = modsTab;
