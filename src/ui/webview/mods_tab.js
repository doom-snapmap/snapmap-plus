/* The Mods tab: the installed mods, grouped by where they came from, and the selected mod's details,
   with the few actions a player uses (install a folder, open the mods folder, remove, rebuild or
   verify a mod, migrate the old overrides folder). Browser global + CommonJS test export. Pure ASCII.

   The backend (backend/modui.c, the `mods` interface slot) builds one document on the game thread:
   one row per mod folder in mods\ (its key is the folder name; two folders declaring one id are both
   rows, marked duplicateId), whether overrides\ still holds entries to migrate, and the open map
   (which this tab no longer shows). Payload v3 adds, per mod, what the details pane describes:
   description, origin, migratedFrom, servedBy, builtAt, bundle, counts, requirements,
   editorRowNames, carriedPaths, publishedMaps and scope (docs/webview-ui.md). Every v3 field is optional:
   a v2 document renders with those parts left out. The host posts the document as {kind:'mods', data}
   after every action; showing the tab asks for a rescan. Nothing polls. The list is a plain list the
   page scrolls natively. All text goes through textContent. */
var modsTab = (function () {
  var STATES = {
    installed: {label: 'Installed', tone: 'ok'},
    attention: {label: 'Needs attention', tone: 'warn'}
  };
  var DUPLICATE_TEXT = 'another installed mod declares this id; change one id in its mod.json';
  /* The three row states: each backend state and cause maps onto exactly one. */
  var STATUS = {
    ok:   {icon: 'check', label: 'Installed and serving'},
    warn: {icon: 'triangle-alert', label: 'Needs attention'},
    info: {icon: 'circle-info', label: 'Installed; another build serves its files'}
  };
  var ORIGINS = {
    '':        'You added this folder',
    install:   'Installed from a folder',
    migration: 'Moved from the overrides folder',
    download:  'Downloaded for a map',
    delivery:  'Delivered by a map'
  };
  var GROUPS = [
    {id: 'yours', label: 'Your mods'},
    {id: 'installed', label: 'Installed'},
    {id: 'maps', label: 'From maps you played'}
  ];
  var COUNT_LINES = [
    ['shadows', 'Replaces %s'], ['sounds', '%s'], ['movies', '%s'], ['modules', '%s'], ['vt', '%s']
  ];
  var COUNT_NOUNS = {shadows: ['stock file', 'stock files'], sounds: ['sound', 'sounds'], movies: ['movie', 'movies'],
                     modules: ['module', 'modules'], vt: ['virtual texture', 'virtual textures']};
  /* remove is the v2 name of uninstall; the page sends uninstall. The two *Preview actions are dry
     runs that answer with the consequence list the confirmation shows. mapAddMod attaches the
     selected mod to the open map without placing anything (a map-level row); it removes nothing,
     so it has no dry run. Uninstall is permanent: there is no bin to empty or restore from. */
  var ACTIONS = ['install', 'migrate', 'rescan', 'openFolder', 'rebuild', 'verify', 'mapAddMod',
                 'mapRemoveModPreview', 'mapRemoveMod', 'uninstallPreview', 'uninstall'];
  /* What removing a mod from a map severs: wires and timeline references to its placed entities.
     Other reference kinds are not consequences of the removal and are not shown. */
  var REF_KINDS = {wire: 'wire', timeline: 'timeline'};
  var MAP_STATES = {installed: 'ok', active: 'ok', superseded: 'info'};
  /* A map needs the paths its rows list; a mod folder provides them (the asset rule). */
  var MAP_CAUSES = {missing: 'not installed on this machine', incomplete: 'installed, but it lacks files this map uses',
                    unindexed: 'not scanned yet; reopen the Mods tab', skipped: 'installed but not in use',
                    attention: 'needs attention'};
  /* Under the Uninstall confirmation's scope line: it cannot be undone. */
  var UNINSTALL_PERMANENT = 'This cannot be undone. Uninstalling:';
  /* The saved maps that use the mod (the dry run's savedMaps), and the undo history. */
  function savedMapsText(n) {
    return n + ' of your saved maps use it; they will not open until a mod folder provides its assets again.';
  }
  var UNDO_CLEARED = 'Undo history is cleared.';
  /* The scope sentence under each confirmation's title, so the two dialogs read differently. */
  var SCOPE = {
    mapRemoveMod: 'Only this map changes. The mod stays installed on this computer and keeps its Toybox rows.',
    uninstallUsed: 'Removes the mod from this computer. The open map is cleaned of it first.',
    uninstallUnused: 'Removes the mod from this computer. The open map does not use it.'
  };
  function confirmScope(pv) { return pv.action === 'uninstall' ? (pv.map ? SCOPE.uninstallUsed : SCOPE.uninstallUnused) : SCOPE.mapRemoveMod; }
  var COLLAPSE_KEY = 'mods.collapsed_groups';
  var FILES_NOTE = "Files identical to the game's own are never stored or uploaded; Snapmap+ reads them from your install.";
  var NEEDS_NOTE = 'Every player needs Snapmap+ to play a map that uses this mod.';
  /* Where a mod's replacements and requirements apply (payload scope; docs/webview-ui.md). */
  var SCOPE_TEXT = {
    map: 'Only in maps that use it',
    always: 'In every map',
    auto: 'In every map: it changes the editor itself'
  };
  var USE_TITLE = 'Use the selected mod in the whole open map without placing anything; the map lists it when you save';
  var MAP_LEVEL_TEXT = 'Used by the whole map';

  function str(v) { return typeof v === 'string' ? v : (v === null || v === undefined ? '' : String(v)); }
  function num(v) { var n = Number(v); return isFinite(n) ? n : 0; }
  function arr(v) { return Array.isArray(v) ? v : []; }
  function bool(v) { return v === true; }
  function obj(v) { return v && typeof v === 'object' && !Array.isArray(v) ? v : null; }
  function has(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }

  function normNames(v) { return arr(v).map(str).filter(function (s) { return s.length > 0; }); }
  /* v3 lists and objects are null when the backend did not send them, so the page can tell
     "none" from "not reported". */
  function normCounts(v) {
    var c = obj(v);
    if (!c) return null;
    return {shadows: num(c.shadows), sounds: num(c.sounds), movies: num(c.movies), modules: num(c.modules), vt: num(c.vt)};
  }
  function normCvars(v) {
    var r = obj(v), c = r && obj(r.cvars);
    if (!c) return null;
    return Object.keys(c).sort().map(function (k) { return {name: k, value: str(c[k])}; });
  }
  /* requirements.commands: console lines run when the mod starts and stops; null when not sent. */
  function normCommands(v) {
    var r = obj(v), c = r && obj(r.commands);
    if (!r || !obj(r.cvars)) return null;
    return {activate: c ? normNames(c.activate) : [], deactivate: c ? normNames(c.deactivate) : []};
  }
  function normMod(b) {
    b = b && typeof b === 'object' ? b : {};
    var state = has(STATES, b.state) ? b.state : 'attention';
    return {key: str(b.key), id: str(b.id), name: str(b.name), author: str(b.author), version: str(b.version),
            build: num(b.build), size: num(b.size), files: num(b.files), pointers: num(b.pointers), state: state,
            cause: str(b.cause), otherBuilds: num(b.otherBuilds), duplicateId: bool(b.duplicateId),
            editorRows: num(b.editorRows), folder: str(b.folder),
            /* v3 */
            description: str(b.description), origin: typeof b.origin === 'string' && has(ORIGINS, b.origin) ? b.origin : '',
            migratedFrom: str(b.migratedFrom), servedBy: str(b.servedBy), builtAt: num(b.builtAt), bundle: str(b.bundle),
            counts: normCounts(b.counts), cvars: normCvars(b.requirements), commands: normCommands(b.requirements),
            editorRowNames: Array.isArray(b.editorRowNames) ? normNames(b.editorRowNames) : null,
            carriedPaths: Array.isArray(b.carriedPaths) ? normNames(b.carriedPaths) : null,
            publishedMaps: b.publishedMaps === undefined || b.publishedMaps === null ? -1 : num(b.publishedMaps),
            scope: b.scope === 'map' || b.scope === 'always' ? b.scope : '', scopeAuto: bool(b.scopeAuto), scoped: bool(b.scoped)};
  }
  /* The About line for where the mod applies; '' when the backend did not say or nothing is scoped. */
  function scopeText(b) {
    if (b.scope === 'always') return b.scopeAuto ? SCOPE_TEXT.auto : SCOPE_TEXT.always;
    if (b.scope === 'map' && b.scoped) return SCOPE_TEXT.map;
    return '';
  }
  function title(b) { return b.name || b.id || b.key; }

  /* The host's payload as the tab uses it. error is set for an unusable payload. The open map
     (payload.map) is not this tab's any more and is not read. */
  function normalize(payload) {
    if (!payload || typeof payload !== 'object' || Array.isArray(payload))
      return {ok: false, error: 'The mods list is unavailable.', mods: [], root: '', gen: 0, v: 0,
              migrate: {entries: 0, pending: 0, running: false}};
    var mg = payload.migrate && typeof payload.migrate === 'object' ? payload.migrate : {};
    var mods = arr(payload.mods).map(normMod);
    mods.sort(function (a, b) {
      var x = title(a).toLowerCase(), y = title(b).toLowerCase();
      return x === y ? (a.key < b.key ? -1 : a.key > b.key ? 1 : 0) : (x < y ? -1 : 1);
    });
    return {ok: true, error: payload.storeReadable === false ? 'The mods folder cannot be read.' : '',
            root: str(payload.root), gen: num(payload.gen), v: num(payload.v), mods: mods,
            migrate: {entries: num(mg.entries), pending: num(mg.pending), running: bool(mg.running)},
            map: normMap(payload.map)};
  }

  /* The open map (payload v3 map object). null: no map object at all; open false: no map is open
     (v2's entities -1). Absent v3 fields stay null and are left out of the modal. */
  function normRef(r) { r = obj(r) || {}; return {kind: str(r.kind), name: str(r.name)}; }
  function normRefs(v) { return arr(v).map(normRef).filter(function (r) { return has(REF_KINDS, r.kind); }); }
  function normMapMod(m) {
    m = obj(m) || {};
    var names = normNames(m.placedEntities);
    return {key: str(m.key), id: str(m.id), version: str(m.version), state: str(m.state) || 'missing',
            placedEntities: names, placedCount: Math.max(num(m.placedCount), names.length),
            references: normRefs(m.references), size: num(m.size), attached: bool(m.attached)};
  }
  function normMap(m) {
    m = obj(m);
    if (!m) return null;
    var open = m.entities !== -1 || !!(m.name || m.id);
    var pub = obj(m.published), up = obj(m.uploaded);
    return {open: open, name: str(m.name), id: str(m.id), saved: num(m.saved), entities: num(m.entities),
            published: pub ? {state: str(pub.state), at: num(pub.at)} : null,
            uploaded: up ? {claimed: bool(up.claimed), at: num(up.at)} : null,
            tagged: typeof m.tagged === 'boolean' ? m.tagged : null,
            mods: Array.isArray(m.mods) ? m.mods.map(normMapMod) : null,
            vanilla: typeof m.vanilla === 'boolean' ? m.vanilla : null};
  }
  /* A dry run's consequence list (mapRemoveModPreview, uninstallPreview). */
  function normPreview(v) {
    v = obj(v);
    if (!v) return null;
    var mp = obj(v.map), ins = obj(v.install), names = mp ? normNames(mp.entities) : [];
    return {action: str(v.action), key: str(v.key), name: str(v.name),
            map: mp ? {name: str(mp.name), entities: names, entityCount: Math.max(num(mp.entityCount), names.length),
                       references: normRefs(mp.references), manifestRow: bool(mp.manifestRow), attached: bool(mp.attached),
                       tagRemoved: bool(mp.tagRemoved), vanillaAfter: bool(mp.vanillaAfter), published: bool(mp.published)} : null,
            /* Uninstall deletes the folder; savedMaps is -1 when the saves could not be read. */
            install: ins ? {folder: str(ins.folder), disposition: 'delete',
                            savedMaps: ins.savedMaps === undefined || ins.savedMaps === null ? -1 : num(ins.savedMaps),
                            undoCleared: bool(ins.undoCleared)} : null,
            /* uninstallPreview only: the Toybox rows uninstalling takes out of the editor */
            editorRows: normNames(v.editorRows)};
  }
  function mapModStatus(r) {
    var tone = MAP_STATES[r.state] || 'warn';
    var cause = tone === 'ok' ? 'Installed' : tone === 'info' ? 'Installed; another build serves its files' : (MAP_CAUSES[r.state] || r.state);
    return {tone: tone, icon: STATUS[tone].icon, label: tone === 'warn' ? STATUS.warn.label : STATUS[tone].label, cause: cause};
  }
  function refText(r) { return REF_KINDS[r.kind] + ': ' + r.name; }
  /* The consequence lines of a dry run, in the order the confirmation lists them. Each line is
     {text, list?} where list is the names behind a count. */
  function consequenceLines(pv) {
    var out = [];
    if (pv.map) {
      var m = pv.map, where = m.name ? ' from ' + m.name : '';
      if (m.entityCount > 0) out.push({text: 'Deletes ' + plural(m.entityCount, 'placed entity', 'placed entities') + where + '.', list: m.entities});
      if (m.references.length) out.push({text: 'Severs ' + plural(m.references.length, 'wire or timeline reference') + ' to those entities.',
                                         list: m.references.map(refText)});
      if (m.manifestRow) out.push({text: m.attached ? "Removes the mod from the map's list of mods; the whole map used it."
                                                    : "Removes the mod's row from the map's list of mods."});
      if (m.tagRemoved) out.push({text: 'Removes the search tag mods: this is the last mod the map uses.'});
      if (m.vanillaAfter) out.push({text: 'The map then needs no mods and plays without Snapmap+.'});
      if (m.published) out.push({text: 'The published copy on the server is unaffected until you publish the map again.'});
    }
    if (pv.action === 'uninstall' && pv.editorRows.length)
      out.push({text: 'Removes ' + plural(pv.editorRows.length, 'Toybox row') + ' from the editor (' + pv.editorRows.join(', ') + ').'});
    if (pv.install) {
      out.push({text: 'Deletes the folder mods\\' + pv.install.folder + ' and its files.'});
      if (pv.install.savedMaps > 0) out.push({text: savedMapsText(pv.install.savedMaps)});
      if (pv.install.undoCleared) out.push({text: UNDO_CLEARED});
    }
    return out;
  }

  function formatSize(bytes) {
    var n = num(bytes);
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
    if (n < 1024 * 1024 * 1024) return (n / 1048576).toFixed(n < 10485760 ? 1 : 0) + ' MB';
    return (n / 1073741824).toFixed(1) + ' GB';
  }
  function plural(n, one, many) { return n + ' ' + (n === 1 ? one : (many || one + 's')); }
  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  /* Local "YYYY-MM-DD HH:MM" from Unix seconds; '' when unknown. */
  function formatDate(sec) {
    var s = num(sec);
    if (s <= 0) return '';
    var d = new Date(s * 1000);
    return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) + ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes());
  }
  /* id@version, the name a map records (just the id when the page is given no version); the folder
     for a mod that was never built. */
  function idText(b) { return b.id ? (b.version ? b.id + '@' + b.version : b.id) : b.key; }
  /* The row's second line: id, version and size, each left out when there is none. */
  function subText(b) {
    if (!b.id) return b.key;
    var parts = [b.id];
    if (b.version) parts.push(b.version);
    parts.push(formatSize(b.size));
    return parts.join(' \u00b7 ');
  }

  /* One of the three states, its one-line cause (the icon's hover title) and the details sentence. */
  function statusInfo(b) {
    var cause;
    if (b.duplicateId || b.state === 'attention') {
      cause = b.duplicateId ? DUPLICATE_TEXT : (b.cause || 'not in use');
      return {tone: 'warn', icon: STATUS.warn.icon, label: STATUS.warn.label, cause: cause,
              sentence: 'Needs attention: ' + cause};
    }
    if (b.servedBy) {
      cause = 'another build of this mod (' + b.servedBy + ') serves its files';
      return {tone: 'info', icon: STATUS.info.icon, label: STATUS.info.label, cause: cause,
              sentence: 'Installed; ' + cause + '.'};
    }
    cause = servingText(b);
    return {tone: 'ok', icon: STATUS.ok.icon, label: STATUS.ok.label, cause: cause, sentence: cause};
  }
  /* Carried files only (files minus stock pointers), the pointers as a second clause. */
  function servingText(b) {
    var carried = Math.max(0, num(b.files) - num(b.pointers)), p = num(b.pointers);
    return 'Installed and serving ' + plural(carried, 'file') + (p > 0 ? ', ' + p + ' more read from your install' : '');
  }
  function needsAttention(b) { return statusInfo(b).tone === 'warn'; }

  /* Your mods: folders the author made, and the migrated overrides (the owner's own). */
  function groupOf(b) {
    if (b.origin === 'install') return 'installed';
    if (b.origin === 'download' || b.origin === 'delivery') return 'maps';
    return 'yours';
  }
  function originText(b) {
    if (b.origin === 'migration') return b.migratedFrom ? 'Moved from ' + b.migratedFrom : ORIGINS.migration;
    return ORIGINS[b.origin] || ORIGINS[''];
  }

  function filterMods(mods, query) {
    var q = str(query).trim().toLowerCase();
    if (!q) return mods.slice();
    return mods.filter(function (b) { return (b.name + '\n' + b.id + '\n' + b.key).toLowerCase().indexOf(q) !== -1; });
  }
  /* The groups in display order, empty ones left out. */
  function groupMods(mods) {
    return GROUPS.map(function (g) {
      return {id: g.id, label: g.label, mods: mods.filter(function (b) { return groupOf(b) === g.id; })};
    }).filter(function (g) { return g.mods.length > 0; });
  }
  /* "Replaces 500 stock files", "9 sounds", ...; zero counts are left out. */
  function countLines(c) {
    if (!c) return [];
    var out = [];
    COUNT_LINES.forEach(function (p) {
      var n = num(c[p[0]]);
      if (n > 0) out.push(p[1].replace('%s', plural(n, COUNT_NOUNS[p[0]][0], COUNT_NOUNS[p[0]][1])));
    });
    return out;
  }

  function actionMessage(action, arg) {
    if (ACTIONS.indexOf(action) < 0) return null;
    var m = {cmd: 'modsAction', action: action};
    if (arg !== undefined && arg !== null && arg !== '') m.arg = str(arg);
    return m;
  }

  /* ------------------------------------------------------------------------------------------ DOM */

  /* env: {document, byId(id), post(message), toast(text, kind), saveCollapsed?(list of group ids)} */
  function create(env) {
    var doc = env.document, byId = env.byId;
    var model = normalize(null), loaded = false, waiting = false, busy = '', quiet = false, selectedKey = '';
    var view = [], filter = '', attnOnly = false, lastError = '', rowNodes = {};
    var collapsed = {}, filterCollapsed = {};
    /* The map modal: open, which page ('list' or 'confirm'), the dry run the confirmation shows,
       what opened the confirmation ('map' or 'uninstall') and the element focus returns to. */
    var modal = {open: false, page: 'list', preview: null, from: 'map', opener: null}, modalButtons = [];

    /* The collapsed groups are a setting (COLLAPSE_KEY) kept by the host's configuration service,
       like the Prefabs tab's open folders: the page saves the list on each toggle and applies the
       stored one when it arrives (loadCollapsed). A bad value starts with every group open. */
    function collapsedFrom(list) {
      var out = {};
      arr(list).forEach(function (id) { if (typeof id === 'string' && id) out[id] = true; });
      return out;
    }
    function writeCollapsed() {
      if (env.saveCollapsed) env.saveCollapsed(Object.keys(collapsed).sort());
    }

    function el(tag, cls, text) {
      var n = doc.createElement(tag);
      if (cls) n.className = cls;
      if (text !== undefined && text !== null) n.textContent = str(text);
      return n;
    }
    function icon(name) {
      var ns = 'http://www.w3.org/2000/svg';
      var svg = doc.createElementNS ? doc.createElementNS(ns, 'svg') : doc.createElement('svg');
      var use = doc.createElementNS ? doc.createElementNS(ns, 'use') : doc.createElement('use');
      svg.setAttribute('class', 'ui-icon');
      svg.setAttribute('aria-hidden', 'true');
      svg.setAttribute('focusable', 'false');
      use.setAttribute('href', '#icon-' + name);
      svg.appendChild(use);
      return svg;
    }
    function statusIcon(info) {
      var s = el('span', 'mods-status-icon ' + info.tone);
      s.setAttribute('role', 'img');
      s.setAttribute('aria-label', info.label);
      s.setAttribute('data-status', info.tone);
      s.appendChild(icon(info.icon));
      return s;
    }
    function clear(n) { while (n && n.firstChild) n.removeChild(n.firstChild); }

    function setStatus(text, tone) {
      var s = byId('modsStatus');
      if (!s) return;
      s.textContent = str(text);
      s.className = 'mods-status' + (tone ? ' ' + tone : '');
    }
    var BUSY_TEXT = {install: 'Installing...', remove: 'Removing...', rescan: 'Reading the mods folder...',
                     rebuild: 'Building the mod again...', verify: 'Checking every file of the mod...',
                     mapRemoveModPreview: 'Reading what removing it changes...', uninstallPreview: 'Reading what uninstalling it changes...',
                     mapRemoveMod: 'Removing the mod from the map...', uninstall: 'Uninstalling...',
                     mapAddMod: 'Adding the mod to the map...',
                     migrate: 'Migrating the old overrides folder; this can take a minute...'};

    function selectedMod() {
      for (var i = 0; i < model.mods.length; i++) if (model.mods[i].key === selectedKey) return model.mods[i];
      return null;
    }
    function attentionCount() { return model.mods.filter(needsAttention).length; }
    function filtering() { return attnOnly || !!str(filter).trim(); }
    function groupOpen(id) { return filtering() ? !filterCollapsed[id] : !collapsed[id]; }

    function renderButtons() {
      var sel = !!selectedMod();
      ['modsInstallBtn', 'modsFolderBtn', 'modsMigrateBtn', 'modsRefreshBtn', 'modsUninstallBtn', 'modsRebuildBtn', 'modsVerifyBtn'].forEach(function (id) {
        var b = byId(id);
        if (b) b.disabled = !!busy || (!sel && (id === 'modsUninstallBtn' || id === 'modsRebuildBtn' || id === 'modsVerifyBtn'));
      });
      var mb = byId('modsMapBtn');
      if (mb) mb.disabled = !loaded;
      renderUse();
      modalButtons.forEach(function (n) { n.disabled = !!busy; });
      var mg = byId('modsMigrateBtn');
      if (mg) {
        var show = loaded && model.migrate.pending > 0 && !model.migrate.running;
        mg.style.display = show || busy === 'migrate' ? '' : 'none';
        mg.title = 'Migrate: move ' + plural(model.migrate.pending, 'entry', 'entries') +
                   ' of the old overrides folder into the mods folder';
      }
    }
    /* The details head's map action: Use in this map, or Remove from map once the open map uses the
       selected mod. Shown only while a map is open in the editor. */
    function mapUses(b) {
      var m = model.map;
      if (!b || !m || !m.mods) return null;
      for (var i = 0; i < m.mods.length; i++) if (m.mods[i].key && m.mods[i].key === b.key) return m.mods[i];
      return null;
    }
    function renderUse() {
      var u = byId('modsUseBtn');
      if (!u) return;
      var b = selectedMod(), m = model.map, open = loaded && !!m && m.open && !!m.mods;
      u.style.display = open ? '' : 'none';
      var used = mapUses(b);
      u.textContent = used ? 'Remove from map' : 'Use in this map';
      u.className = 'btn' + (used ? ' danger' : '');
      u.title = used ? 'Remove the selected mod and everything that uses it from this map' : USE_TITLE;
      u.disabled = !!busy || !b || !open || (!used && (needsAttention(b) || !b.id));
    }
    function setBusy(action) {
      busy = action || '';
      renderButtons();
      if (busy && !(busy === 'rescan' && quiet)) setStatus(BUSY_TEXT[busy] || 'Working...', 'busy');
    }

    /* ---- the list: groups of plain rows; every row of an open group is mounted */
    function computeView() {
      var shown = filterMods(model.mods, filter);
      if (attnOnly) shown = shown.filter(needsAttention);
      var groups = groupMods(shown);
      view = [];
      groups.forEach(function (g) { if (groupOpen(g.id)) view = view.concat(g.mods); });
      return groups;
    }
    function renderList() {
      var box = byId('modsList');
      if (!box) return;
      clear(box);
      rowNodes = {};
      var groups = computeView();
      if (!loaded) { box.appendChild(el('div', 'list-empty', waiting ? 'Reading installed mods...' : 'Open this tab to list installed mods.')); return; }
      if (lastError) { box.appendChild(el('div', 'list-empty mods-error', lastError)); return; }
      if (!model.mods.length) {
        box.appendChild(el('div', 'list-empty', 'No mods installed. Drop a mod folder into mods\\ or use the download button to install one.'));
        return;
      }
      if (!groups.length) {
        box.appendChild(el('div', 'list-empty', attnOnly ? 'No mod that needs attention matches the filter.' : 'No installed mod matches the filter.'));
        return;
      }
      groups.forEach(function (g) { box.appendChild(groupNode(g)); });
    }
    function groupNode(g) {
      var open = groupOpen(g.id);
      var wrap = el('div', 'pf-folder mods-group' + (open ? ' pf-open' : ''));
      wrap.setAttribute('role', 'group');
      wrap.setAttribute('aria-label', g.label);
      wrap.setAttribute('data-group', g.id);
      var head = el('div', 'pf-folder-head mods-group-head');
      head.setAttribute('role', 'button');
      head.setAttribute('tabindex', '0');
      head.setAttribute('aria-expanded', open ? 'true' : 'false');
      head.setAttribute('data-group', g.id);
      var caret = el('span', 'pf-caret');
      caret.appendChild(icon('chevron-down'));
      head.appendChild(caret);
      head.appendChild(el('span', 'pf-fname', g.label));
      head.appendChild(el('span', 'pf-fcount', String(g.mods.length)));
      head.addEventListener('click', function () { toggleGroup(g.id); });
      head.addEventListener('keydown', function (e) {
        if (e.key !== 'Enter' && e.key !== ' ') return;
        if (e.preventDefault) e.preventDefault();
        if (e.stopPropagation) e.stopPropagation();
        toggleGroup(g.id);
      });
      wrap.appendChild(head);
      if (open) {
        var body = el('div', 'mods-group-body');
        g.mods.forEach(function (b) { body.appendChild(modRow(b)); });
        wrap.appendChild(body);
      }
      return wrap;
    }
    function toggleGroup(id) {
      if (filtering()) { if (filterCollapsed[id]) delete filterCollapsed[id]; else filterCollapsed[id] = true; }
      else { if (collapsed[id]) delete collapsed[id]; else collapsed[id] = true; writeCollapsed(); }
      renderList();
    }
    function modRow(b) {
      var info = statusInfo(b), on = b.key === selectedKey;
      var row = el('div', 'mods-row' + (on ? ' selected' : ''));
      row.setAttribute('role', 'option');
      row.setAttribute('data-key', b.key);
      row.setAttribute('aria-selected', on ? 'true' : 'false');
      row.title = info.cause;
      row.appendChild(statusIcon(info));
      var text = el('div', 'mods-row-text');
      text.appendChild(el('div', 'mods-row-name', title(b)));
      text.appendChild(el('div', 'mods-row-sub', subText(b)));
      row.appendChild(text);
      row.addEventListener('click', function () { select(b.key); });
      rowNodes[b.key] = row;
      return row;
    }

    function select(key) {
      selectedKey = key;
      /* Restyle in place: this keeps the scroll anchor. */
      Object.keys(rowNodes).forEach(function (k) {
        var r = rowNodes[k], on = k === key;
        r.className = 'mods-row' + (on ? ' selected' : '');
        r.setAttribute('aria-selected', on ? 'true' : 'false');
        if (on && r.scrollIntoView) r.scrollIntoView({block: 'nearest'});
      });
      renderDetail();
      renderButtons();
    }
    /* Arrow keys, Home and End move the selection over the rows shown (collapsed groups are
       skipped); Page Up and Page Down scroll natively. */
    function onListKey(e) {
      var keys = {ArrowDown: 1, ArrowUp: -1, Home: -Infinity, End: Infinity};
      if (!view.length || !has(keys, e.key)) return;
      var at = -1;
      for (var i = 0; i < view.length; i++) if (view[i].key === selectedKey) at = i;
      var step = keys[e.key];
      var next = step === -Infinity ? 0 : step === Infinity ? view.length - 1 : at < 0 ? 0 : Math.max(0, Math.min(view.length - 1, at + step));
      if (e.preventDefault) e.preventDefault();
      select(view[next].key);
    }

    /* ---- the selected mod */
    function section(box, name, label) {
      var s = el('section', 'mods-section');
      s.setAttribute('data-section', name);
      s.appendChild(el('div', 'mods-section-head', label));
      box.appendChild(s);
      return s;
    }
    function field(box, label, value) {
      var r = el('div', 'mods-field');
      r.appendChild(el('span', 'mods-field-label', label));
      var v = typeof value === 'object' && value !== null ? value : el('span', 'mods-field-value', value);
      if (v.className.indexOf('mods-field-value') < 0) v.className += ' mods-field-value';
      r.appendChild(v);
      box.appendChild(r);
      return r;
    }
    function howTo(box, empty) {
      var card = el('div', 'mods-howto');
      card.appendChild(el('div', 'mods-howto-head', 'How mods work'));
      card.appendChild(el('p', '', 'A mod is a folder in mods\\ holding mod.json and an assets\\ folder.'));
      card.appendChild(el('p', '', 'Maps record which mods they use. Players who load one are offered the download.'));
      card.appendChild(el('p', '', empty ? 'To add a mod, drop a folder into mods\\ or use the download button.'
                                         : 'Select a mod to see what it adds.'));
      box.appendChild(card);
    }
    function renderDetail() {
      var box = byId('modsDetail');
      if (!box) return;
      clear(box);
      var b = selectedMod();
      if (!b) { if (loaded && !lastError) howTo(box, !model.mods.length); return; }
      var info = statusInfo(b);

      var head = el('div', 'mods-detail-head');
      var ic = statusIcon(info);
      ic.title = info.cause;
      head.appendChild(ic);
      head.appendChild(el('span', 'mods-detail-name', title(b)));
      box.appendChild(head);
      box.appendChild(el('div', 'mods-detail-sentence ' + info.tone, info.sentence));

      var about = section(box, 'about', 'About');
      field(about, 'Id', b.id || '(not built yet)');
      if (b.version) field(about, 'Version', b.version);
      if (b.build > 0) field(about, 'Build', String(b.build));
      if (b.author) field(about, 'Author', b.author);
      if (b.description) field(about, 'Description', b.description).className += ' mods-field-long';
      var link = el('button', 'mods-link', b.folder || b.key);
      link.type = 'button';
      link.title = 'Open this folder in Explorer';
      link.addEventListener('click', function () { act('openFolder', b.folder || b.key); });
      field(about, 'Folder', link);
      field(about, 'Origin', originText(b));
      var applies = scopeText(b);
      if (applies) field(about, 'Applies', applies);

      var adds = section(box, 'adds', 'What it adds');
      var names = b.editorRowNames || [];
      if (names.length) {
        var list = el('div', 'mods-rows-list');
        names.forEach(function (n) { list.appendChild(el('div', 'mods-rows-item', n)); });
        field(adds, plural(names.length, 'Toybox row'), list);
      } else if (b.editorRows > 0) {
        adds.appendChild(el('div', 'mods-line', plural(b.editorRows, 'Toybox row')));
      }
      countLines(b.counts).forEach(function (t) { adds.appendChild(el('div', 'mods-line', t)); });
      if (adds.children.length === 1) adds.appendChild(el('div', 'mods-line muted', 'Adds no Toybox rows.'));

      var files = section(box, 'files', 'Files');
      var carried = Math.max(0, b.files - b.pointers);
      field(files, 'Carried', plural(carried, 'file') + ', ' + formatSize(b.size));
      if (b.pointers > 0) field(files, 'From the game', b.pointers + ' more read from your install');
      files.appendChild(el('div', 'mods-note', FILES_NOTE));
      if (b.carriedPaths && b.carriedPaths.length) {
        var det = el('details', 'pc-limits-details mods-paths');
        det.appendChild(el('summary', '', 'Carried paths (' + carried + ')'));
        var pl = el('div', 'mods-paths-list');
        b.carriedPaths.forEach(function (p) { pl.appendChild(el('div', '', p)); });
        if (carried > b.carriedPaths.length) pl.appendChild(el('div', 'muted', 'and ' + (carried - b.carriedPaths.length) + ' more'));
        det.appendChild(pl);
        files.appendChild(det);
      }

      var req = section(box, 'requirements', 'Requirements');
      if (b.cvars && b.cvars.length) {
        var cl = el('div', 'mods-rows-list mono');
        b.cvars.forEach(function (c) { cl.appendChild(el('div', '', c.name + ' ' + c.value)); });
        field(req, 'Console settings', cl);
      } else if (b.cvars) {
        field(req, 'Console settings', 'None');
      }
      [['activate', 'Commands when active'], ['deactivate', 'Commands when removed']].forEach(function (k) {
        var lines = b.commands ? b.commands[k[0]] : [];
        if (!lines.length) return;
        var ll = el('div', 'mods-rows-list mono');
        lines.forEach(function (line) { ll.appendChild(el('div', '', line)); });
        field(req, k[1], ll);
      });
      req.appendChild(el('div', 'mods-note', NEEDS_NOTE));

      var lines = [];
      if (b.publishedMaps >= 0) lines.push(['Published maps', b.publishedMaps ? 'Used by ' + plural(b.publishedMaps, 'of your published maps', 'of your published maps') : 'Used by none of your published maps']);
      var when = formatDate(b.builtAt);
      if (when) lines.push(['Last built', when]);
      if (b.bundle) lines.push(['Bundle', b.bundle.slice(0, 16)]);
      if (lines.length) {
        var share = section(box, 'sharing', 'Sharing');
        lines.forEach(function (l) {
          var r = field(share, l[0], l[1]);
          if (l[0] === 'Bundle') { r.title = 'Quote this when you ask for help with this mod'; r.className += ' mods-bundle'; }
        });
        if (b.bundle) share.appendChild(el('div', 'mods-note', 'Quote the bundle number when you ask for help with this mod.'));
      }
    }

    /* ---- the map modal: the open map's details and the mods it uses. It reuses the decl editor's
       focus modal (overlay shown with display flex, aria-hidden, backdrop and Escape close it) and
       renders only from the last document and the last dry run, never from its own bookkeeping. */
    function modalButton(text, cls, fn, label) {
      var b = el('button', 'btn' + (cls ? ' ' + cls : ''), text);
      b.type = 'button';
      if (label) { b.title = label; b.setAttribute('aria-label', label); }
      b.disabled = !!busy;
      b.addEventListener('click', fn);
      modalButtons.push(b);
      return b;
    }
    function modName(key, id) {
      for (var i = 0; i < model.mods.length; i++) if (key && model.mods[i].key === key) return title(model.mods[i]);
      return id || key;
    }
    /* keep: open on the page already set (a confirmation from the details pane). */
    function openMap(opener, keep) {
      modal.open = true;
      if (!keep) { modal.page = 'list'; modal.preview = null; modal.from = 'map'; }
      if (opener !== undefined) modal.opener = opener;
      showOverlay(true);
      renderModal();
      if (modal.closeBtn && modal.closeBtn.focus) modal.closeBtn.focus();
    }
    function closeMap() {
      if (!modal.open) return;
      modal.open = false;
      modal.page = 'list';
      modal.preview = null;
      showOverlay(false);
      var host = byId('modsMapHost');
      clear(host);
      modalButtons = [];
      if (modal.opener && modal.opener.focus) modal.opener.focus();
      modal.opener = null;
    }
    function showOverlay(on) {
      var ov = byId('modsMapModal');
      if (!ov) return;
      ov.style.display = on ? 'flex' : 'none';
      ov.setAttribute('aria-hidden', on ? 'false' : 'true');
    }
    function modalHead(host, text) {
      var head = el('div', 'mods-map-head');
      var t = el('div', 'modal-title', text);
      t.id = 'modsMapTitle';
      head.appendChild(t);
      var x = el('button', 'btn icon icon-button mods-map-close');
      x.type = 'button';
      x.title = 'Close (Esc)';
      x.setAttribute('aria-label', 'Close');
      x.appendChild(icon('x'));
      x.addEventListener('click', closeMap);
      head.appendChild(x);
      modal.closeBtn = x;
      host.appendChild(head);
    }
    function renderModal() {
      var host = byId('modsMapHost');
      if (!host || !modal.open) return;
      clear(host);
      modalButtons = [];
      if (modal.page === 'confirm' && modal.preview) { renderConfirm(host, modal.preview); return; }
      var m = model.map;
      modalHead(host, m && m.open && m.name ? m.name : 'This map');
      var body = el('div', 'mods-map-body');
      host.appendChild(body);
      if (!m || !m.open) {
        body.appendChild(el('div', 'mods-note', 'Open a map in the editor to see the mods it uses.'));
        return;
      }
      var det = section(body, 'map', 'Map details');
      if (m.name) field(det, 'Name', m.name);
      if (m.id) field(det, 'Saved map', m.id.slice(0, 16)).className += ' mods-bundle';
      if (m.published) {
        var pubd = m.published.state === 'published';
        field(det, 'Published', pubd ? (m.published.at ? 'Yes, ' + formatDate(m.published.at) : 'Yes') : 'Not published');
      }
      if (m.uploaded) field(det, 'Mod store', m.uploaded.claimed
        ? 'Its mods are uploaded' + (m.uploaded.at ? ' (' + formatDate(m.uploaded.at) + ')' : '')
        : 'Its mods are not uploaded');
      if (m.tagged !== null) field(det, 'Search tag', m.tagged ? 'mods (present)' : 'mods (absent)');
      if (m.mods) {
        var total = m.mods.reduce(function (a, r) { return a + r.size; }, 0);
        field(det, 'Mods', plural(m.mods.length, 'mod') + (m.mods.length ? ', ' + formatSize(total) + ' for a player to download' : ''));
      }
      if (m.saved) field(det, 'Last saved', formatDate(m.saved));
      if (!m.mods) return;
      var list = section(body, 'mapMods', 'Mods in this map');
      if (!m.mods.length) {
        list.appendChild(el('div', 'mods-vanilla', m.vanilla === false ? 'This map lists no mods.'
          : 'This map is now vanilla: it uses no mods and plays without Snapmap+.'));
        return;
      }
      m.mods.forEach(function (r) { list.appendChild(mapModRow(r)); });
    }
    function mapModRow(r) {
      var info = mapModStatus(r);
      var row = el('div', 'mods-map-row');
      row.setAttribute('data-key', r.key || r.id);
      row.title = info.cause;
      row.appendChild(statusIcon(info));
      var text = el('div', 'mods-row-text');
      text.appendChild(el('div', 'mods-row-name', modName(r.key, r.id)));
      text.appendChild(el('div', 'mods-row-sub', (r.version ? r.id + ' \u00b7 ' + r.version : r.id) + ' \u00b7 ' + formatSize(r.size)));
      var uses = r.attached
        ? MAP_LEVEL_TEXT + (r.placedCount ? ', and ' + plural(r.placedCount, 'placed entity', 'placed entities') + (r.placedCount === 1 ? ' uses it' : ' use it') : '')
        : plural(r.placedCount, 'placed entity', 'placed entities') + (r.placedCount === 1 ? ' uses it' : ' use it');
      text.appendChild(el('div', 'mods-map-uses' + (r.attached ? ' mods-map-level' : ''), uses + (info.tone === 'warn' ? '; ' + info.cause : '')));
      if (r.attached) row.setAttribute('data-attached', 'true');
      if (r.references.length) text.appendChild(el('div', 'mods-map-uses', 'Also used by ' + r.references.map(refText).join(', ')));
      row.appendChild(text);
      var name = modName(r.key, r.id);
      row.appendChild(modalButton('Remove from map', 'danger', function () {
        modal.from = 'map';
        act('mapRemoveModPreview', r.key || r.id);
      }, 'Remove ' + name + ' and everything that uses it from this map'));
      return row;
    }
    function renderConfirm(host, pv) {
      var name = pv.name || modName(pv.key, pv.key);
      var uninstall = pv.action === 'uninstall';
      modalHead(host, uninstall ? 'Uninstall ' + name + '?' : 'Remove ' + name + ' from ' + (pv.map && pv.map.name ? pv.map.name : 'this map') + '?');
      var body = el('div', 'mods-map-body');
      host.appendChild(body);
      body.appendChild(el('div', 'mods-note mods-scope', confirmScope(pv)));
      body.appendChild(el('div', 'mods-note', uninstall
        ? UNINSTALL_PERMANENT
        : 'Ctrl+Z in the editor brings the entities back, also after a Play round. Removing it:'));
      var ul = el('ul', 'mods-consequences');
      consequenceLines(pv).forEach(function (c) {
        var li = el('li', '', c.text);
        if (c.list && c.list.length) {
          if (c.list.length > 5) {
            var d = el('details', 'pc-limits-details');
            d.appendChild(el('summary', '', 'Show all ' + c.list.length));
            var dl = el('div', 'mods-paths-list mods-names');
            c.list.forEach(function (n) { dl.appendChild(el('div', '', n)); });
            d.appendChild(dl);
            li.appendChild(d);
          } else {
            var sl = el('div', 'mods-consequence-names');
            c.list.forEach(function (n) { sl.appendChild(el('div', '', n)); });
            li.appendChild(sl);
          }
        }
        ul.appendChild(li);
      });
      body.appendChild(ul);
      /* The footer stays at the dialog's bottom edge; the consequence list scrolls above it. */
      var acts = el('div', 'modal-actions mods-map-foot');
      acts.appendChild(modalButton('Cancel', '', function () {
        if (modal.from === 'uninstall' || modal.from === 'details') closeMap();
        else { modal.page = 'list'; modal.preview = null; renderModal(); }
      }));
      acts.appendChild(modalButton(uninstall ? 'Uninstall' : 'Delete from map', 'danger-solid', function () {
        act(uninstall ? 'uninstall' : 'mapRemoveMod', pv.key);
      }));
      host.appendChild(acts);
    }
    /* Escape closes; Tab stays inside the dialog. */
    function onModalKey(e) {
      if (!modal.open) return;
      if (e.key === 'Escape') { if (e.preventDefault) e.preventDefault(); closeMap(); return; }
      if (e.key !== 'Tab') return;
      var host = byId('modsMapHost');
      var f = host && host.querySelectorAll ? Array.prototype.filter.call(host.querySelectorAll('button, summary, [tabindex="0"]'),
        function (n) { return !n.disabled && n.offsetParent !== null; }) : [];
      if (!f.length) return;
      var first = f[0], last = f[f.length - 1], at = doc.activeElement;
      if (e.shiftKey && (at === first || !host.contains(at))) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && (at === last || !host.contains(at))) { e.preventDefault(); first.focus(); }
    }

    function renderHead() {
      var badge = byId('modsBadge');
      if (badge) badge.textContent = String(model.mods.length);
      var n = attentionCount(), at = byId('modsAttnBtn');
      if (n === 0 && attnOnly) { attnOnly = false; filterCollapsed = {}; }
      if (at) {
        clear(at);
        at.appendChild(icon('triangle-alert'));
        at.appendChild(el('span', '', String(n)));
        at.style.display = loaded && n > 0 ? '' : 'none';
        at.setAttribute('aria-pressed', attnOnly ? 'true' : 'false');
        at.className = 'badge mods-attn' + (attnOnly ? ' on' : '');
        at.title = plural(n, 'mod needs', 'mods need') + ' attention: ' + (attnOnly ? 'show every mod' : 'show only these');
        at.setAttribute('aria-label', plural(n, 'mod needs', 'mods need') + ' attention; show only these');
      }
      var rootLine = byId('modsRoot');
      if (rootLine) { rootLine.textContent = model.root; rootLine.title = model.root; }
    }

    function renderAll() {
      if (selectedKey && !selectedMod()) selectedKey = '';
      renderHead();
      renderList();
      renderDetail();
      renderModal();
      renderButtons();
    }
    function setAttnOnly(on) {
      attnOnly = !!on && attentionCount() > 0;
      filterCollapsed = {};
      renderHead();
      renderList();
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
    function onMods(d) {
      waiting = false;
      loaded = true;
      if (!d || d.data === null || d.data === undefined) {
        lastError = d && d.error ? str(d.error) : 'The mods list is unavailable.';
        model = normalize(null);
      } else {
        model = normalize(d.data);
        lastError = model.ok ? model.error : 'The mods list is unavailable.';
        if (!busy) setStatus('', '');
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
      if (action === 'mapRemoveModPreview' || action === 'uninstallPreview') {
        var pv = ok ? normPreview(data.preview) : null;
        setStatus(pv ? '' : message, pv ? '' : 'err');
        if (!pv) { env.toast(message || 'The game could not say what this would change.', 'err'); return; }
        if (action === 'uninstallPreview') { modal.from = 'uninstall'; pv.action = 'uninstall'; }
        else pv.action = 'mapRemoveMod';
        modal.preview = pv;
        modal.page = 'confirm';
        if (!modal.open) openMap(undefined, true); else { renderModal(); if (modal.closeBtn && modal.closeBtn.focus) modal.closeBtn.focus(); }
        return;
      }
      setStatus(message, ok ? 'ok' : 'err');
      if (message) env.toast(message, ok ? 'ok' : 'err');
      if (action === 'install' && ok && data.key) selectedKey = str(data.key);
      if ((action === 'uninstall' || action === 'remove') && ok) selectedKey = '';
      /* After the real action the modal shows the next document: the list, or nothing after an
         uninstall started from the details pane. A refusal goes back the same way. */
      if (action === 'mapRemoveMod' || action === 'uninstall') {
        if (modal.from === 'uninstall' || modal.from === 'details') closeMap();
        else if (modal.open) { modal.page = 'list'; modal.preview = null; renderModal(); }
      }
    }

    /* ---- wiring */
    function bind() {
      var on = function (id, ev, fn) { var n = byId(id); if (n) n.addEventListener(ev, fn); };
      on('modsInstallBtn', 'click', function () { act('install'); });
      on('modsFolderBtn', 'click', function () { act('openFolder'); });
      on('modsMigrateBtn', 'click', function () { act('migrate'); });
      on('modsUninstallBtn', 'click', function () {
        var b = selectedMod();
        if (!b || busy) return;
        modal.opener = byId('modsUninstallBtn');
        act('uninstallPreview', b.key);
      });
      on('modsMapBtn', 'click', function () { openMap(byId('modsMapBtn')); });
      on('modsUseBtn', 'click', function () {
        var b = selectedMod();
        if (!b || busy) return;
        if (mapUses(b)) {
          modal.from = 'details';
          modal.opener = byId('modsUseBtn');
          act('mapRemoveModPreview', b.key);
        } else act('mapAddMod', b.key);
      });
      on('modsMapModal', 'keydown', onModalKey);
      on('modsMapModal', 'mousedown', function (e) { if (e.target === byId('modsMapModal')) closeMap(); });
      on('modsRebuildBtn', 'click', function () { var b = selectedMod(); if (b) act('rebuild', b.key); });
      on('modsVerifyBtn', 'click', function () { var b = selectedMod(); if (b) act('verify', b.key); });
      on('modsRefreshBtn', 'click', function () { refresh(); });
      on('modsAttnBtn', 'click', function () { setAttnOnly(!attnOnly); });
      on('modsFilter', 'input', function () { filter = str(byId('modsFilter').value); filterCollapsed = {}; renderList(); });
      on('modsList', 'keydown', onListKey);
      renderHead();
      renderButtons();
    }

    return {
      bind: bind,
      /* The tab became visible: reread the folder (explicit demand, never a timer). */
      show: function () { refresh(); },
      refresh: refresh,
      act: act,
      select: select,
      setAttentionOnly: setAttnOnly,
      openMap: function () { openMap(); },
      /* The stored COLLAPSE_KEY value: a list of group ids. */
      loadCollapsed: function (list) { collapsed = collapsedFrom(list); renderList(); },
      closeMap: closeMap,
      onMessage: function (d) {
        if (!d || typeof d !== 'object') return false;
        if (d.kind === 'mods') { onMods(d); return true; }
        if (d.kind === 'modsActionResult') { onResult(d); return true; }
        return false;
      },
      state: function () {
        return {model: model, loaded: loaded, waiting: waiting, busy: busy, selectedKey: selectedKey, view: view, error: lastError,
                attentionOnly: attnOnly, collapsed: Object.keys(collapsed).sort(),
                modal: {open: modal.open, page: modal.page, from: modal.from, preview: modal.preview}};
      }
    };
  }

  /* ------------------------------------------------------------------------------- stand-in host */

  /* The browser preview's backend: it holds a copy of a document, answers each modsAction as the
     host does (the action's result, then a fresh document) and changes its copy the way the backend
     would, so the page is exercised by the same render-from-the-document path as in the game.
     handle(message) returns the messages the host would post. Used by mockup.html and the tests. */
  function standIn(initial) {
    var docu = JSON.parse(JSON.stringify(initial || {}));
    function clone() { return JSON.parse(JSON.stringify(docu)); }
    function mods() { return Array.isArray(docu.mods) ? docu.mods : (docu.mods = []); }
    function modOf(key) { return mods().filter(function (b) { return b.key === key; })[0] || null; }
    function mapMods() { return docu.map && Array.isArray(docu.map.mods) ? docu.map.mods : []; }
    function mapModOf(key) { return mapMods().filter(function (r) { return (r.key || r.id) === key; })[0] || null; }
    function nameOf(key, id) { var b = modOf(key); return b ? (b.name || b.id || b.key) : (id || key); }
    function recount() {
      var ids = {};
      mods().forEach(function (b) { if (b.id) ids[b.id] = (ids[b.id] || 0) + 1; });
      mods().forEach(function (b) {
        b.duplicateId = !!b.id && ids[b.id] > 1;
        b.state = b.duplicateId || b.cause ? 'attention' : 'installed';
        if (b.servedBy && !modOf(b.servedBy)) b.servedBy = '';
      });
      if (docu.map && Array.isArray(docu.map.mods)) {
        docu.map.tagged = docu.map.mods.length > 0;
        docu.map.vanilla = docu.map.mods.length === 0;
      }
    }
    function mapPart(r) {
      var left = mapMods().length - 1;
      return {name: str(docu.map && docu.map.name), entities: arr(r.placedEntities).slice(), entityCount: num(r.placedCount),
              references: arr(r.references).slice(), manifestRow: true, attached: !!r.attached,
              tagRemoved: left === 0 && docu.map.tagged !== false,
              vanillaAfter: left === 0, published: !!(docu.map.published && docu.map.published.state === 'published')};
    }
    /* Uninstall deletes the folder; the saved maps are not the preview's to count. */
    function installPart(b, open) {
      return {folder: b.folder || b.key, disposition: 'delete', savedMaps: num(b.savedMaps), undoCleared: !!open};
    }
    function removeFromMap(r) {
      docu.map.mods = mapMods().filter(function (x) { return x !== r; });
      docu.map.entities = Math.max(0, num(docu.map.entities) - num(r.placedCount));
    }
    return function handle(o) {
      var a = str(o && o.action), key = str(o && o.arg), ok = true, msg = '', data = {action: a};
      if (a === 'openFolder') return [];
      var r = mapModOf(key), b = modOf(key);
      if (a === 'rescan') msg = '';
      else if (a === 'rebuild') msg = 'Browser preview: ' + key + ' was built again; nothing changed.';
      else if (a === 'verify') msg = 'Browser preview: every file of ' + key + ' matches its recorded hash.';
      else if (a === 'mapAddMod') {
        if (!docu.map || docu.map.entities === -1 || !Array.isArray(docu.map.mods)) { ok = false; msg = 'No map is open in the editor.'; }
        else if (!b) { ok = false; msg = key + ' is not installed.'; }
        else if (r && r.attached) msg = nameOf(key) + ' is already used by the whole of ' + docu.map.name + '.';
        else {
          if (r) r.attached = true;
          else docu.map.mods.push({key: key, id: str(b.id), version: str(b.version), state: 'installed', placedEntities: [],
                                   placedCount: 0, placedModules: 0, references: [], size: num(b.size), inManifest: true,
                                   viaNeeds: false, attached: true});
          msg = nameOf(key) + ' is now used by the whole of ' + docu.map.name + '.';
        }
      } else if (a === 'mapRemoveModPreview') {
        if (!r) { ok = false; msg = 'The open map does not use ' + key + '.'; }
        else data.preview = {action: 'mapRemoveMod', key: key, name: nameOf(r.key, r.id), map: mapPart(r), install: null};
      } else if (a === 'uninstallPreview') {
        if (!b) { ok = false; msg = key + ' is not installed.'; }
        else data.preview = {action: 'uninstall', key: key, name: nameOf(key), map: r ? mapPart(r) : null,
                             install: installPart(b, !!(docu.map && docu.map.entities !== -1)),
                             editorRows: arr(b.editorRowNames).slice()};
      } else if (a === 'mapRemoveMod') {
        if (!r) { ok = false; msg = 'The open map does not use ' + key + '.'; }
        else { msg = 'Removed ' + nameOf(r.key, r.id) + ' from ' + docu.map.name + ': ' + plural(num(r.placedCount), 'entity', 'entities') + ' deleted.'; removeFromMap(r); }
      } else if (a === 'uninstall') {
        if (!b) { ok = false; msg = key + ' is not installed.'; }
        else {
          var open = !!(docu.map && docu.map.entities !== -1);
          msg = 'Uninstalled ' + nameOf(key) + (r ? ' and removed it from the open map' : '') + '. Its folder was deleted' +
                (open ? ' and the undo history was cleared.' : '.');
          if (r) removeFromMap(r);
          docu.mods = mods().filter(function (x) { return x !== b; });
        }
      } else { ok = false; msg = 'Browser preview: the mods store is not available here.'; }
      recount();
      data.ok = ok;
      data.message = msg;
      return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}, {kind: 'mods', data: clone()}];
    };
  }

  return {STATES: STATES, STATUS: STATUS, ORIGINS: ORIGINS, GROUPS: GROUPS, ACTIONS: ACTIONS, DUPLICATE_TEXT: DUPLICATE_TEXT,
          COLLAPSE_KEY: COLLAPSE_KEY, FILES_NOTE: FILES_NOTE, NEEDS_NOTE: NEEDS_NOTE,
          normalize: normalize, formatSize: formatSize, formatDate: formatDate, statusInfo: statusInfo, servingText: servingText, groupOf: groupOf,
          groupMods: groupMods, originText: originText, countLines: countLines, idText: idText, subText: subText,
          filterMods: filterMods, actionMessage: actionMessage, consequenceLines: consequenceLines, normPreview: normPreview,
          mapModStatus: mapModStatus, UNINSTALL_PERMANENT: UNINSTALL_PERMANENT, UNDO_CLEARED: UNDO_CLEARED, savedMapsText: savedMapsText,
          SCOPE: SCOPE, SCOPE_TEXT: SCOPE_TEXT, MAP_LEVEL_TEXT: MAP_LEVEL_TEXT, scopeText: scopeText, standIn: standIn, create: create};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = modsTab;
