/* The Mods tab: the installed mods in one list sorted by name, and the selected mod's details,
   with the few actions a player uses (install a folder, open the mods folder, remove, rebuild or
   verify a mod, migrate the old overrides folder). Browser global + CommonJS test export. Pure ASCII.

   The backend (backend/modui.c, the `mods` interface slot) builds one document on the game thread:
   one row per mod folder in mods\ (its key is the folder name; two folders holding one build, one key
   at one hash, are both rows, marked duplicateId), whether overrides\ still holds entries to migrate, and the open map
   (the map modal and the rows' in-this-map glyph). Payload v3 adds, per mod, what the details pane describes:
   description, origin, migratedFrom, servedBy, builtAt, bundle, counts, requirements,
   editorRowNames, carriedPaths and publishedMaps, and the additive model's derived facts (facts:
   adds, borrows, surfaces, editor tweaks, replacements with the declared marker, registry contributions),
   and a failed build (docs/webview-ui.md); the page shows no build numbers and no legacy state. Every v3 field is optional:
   a v2 document renders with those parts left out. The host posts the document as {kind:'mods', data}
   after every action; showing the tab asks for a rescan. Nothing polls. The list is plain rows the
   page scrolls natively, a mod in an organizing folder of mods\ under that folder's row; each row
   carries one status glyph: needs attention, else in this map (from the document's map.mods, no read
   of its own), else installed. The Files modal shows a mod folder read-only (modFiles, modFileText),
   and New mod makes a folder with a working example (newMod). All text goes through textContent. */
var modsTab = (function () {
  var STATES = {
    installed: {label: 'Installed', tone: 'ok'},
    attention: {label: 'Needs attention', tone: 'warn'}
  };
  /* duplicateId: another folder holds this key at this hash, an exact copy (modui.c mu_model_rows).
     Two hashes of one key are two builds, never attention (DESIGN-additive 2.2), and a key never changes. */
  var DUPLICATE_TEXT = 'an identical copy of this mod is installed in another folder; uninstall one of the two';
  /* The row states a player sees: installed, or needs attention with its cause (owner 2026-11-09: no
     build management, so a folder another build of its key outranks is simply installed). */
  var STATUS = {
    ok:   {icon: 'check', label: 'Installed and serving'},
    warn: {icon: 'triangle-alert', label: 'Needs attention'}
  };
  /* The details pane's line about where the folder came from (the product block's origin). */
  var ORIGINS = {
    '':        'Added by hand',
    install:   'Imported from an export',
    migration: 'Moved from the old overrides folder',
    download:  'Downloaded for a map you played',
    delivery:  'Downloaded for a map you played'
  };
  var IN_MAP_TEXT = 'In this map';
  var IN_MAP_TITLE = 'The open map uses this mod';
  /* The row's one glyph for a mod the open map uses (installed and in this map). */
  var IN_MAP_GLYPH = {tone: 'map', icon: 'map', label: IN_MAP_TEXT};
  var COUNT_LINES = [
    ['shadows', 'Replaces %s'], ['sounds', '%s'], ['movies', '%s'], ['modules', '%s'], ['vt', '%s']
  ];
  var COUNT_NOUNS = {shadows: ['stock file', 'stock files'], sounds: ['sound', 'sounds'], movies: ['movie', 'movies'],
                     modules: ['module', 'modules'], vt: ['virtual texture', 'virtual textures']};
  /* remove is the v2 name of uninstall; the page sends uninstall. The two *Preview actions are dry
     runs that answer with the consequence list the confirmation shows. mapAddMod attaches the
     selected mod to the open map without placing anything (a map-level row); it removes nothing,
     so it has no dry run. Uninstall moves the folder to the Windows Recycle Bin; the page has no bin
     of its own to empty or restore from. */
  var ACTIONS = ['install', 'migrate', 'rescan', 'openFolder', 'rebuild', 'verify', 'mapAddMod',
                 'mapRemoveModPreview', 'mapRemoveMod', 'uninstallPreview', 'uninstall', 'chooseRegistry',
                 'modFiles', 'modFileText', 'newMod'];
  /* The Files view's two reads change nothing: the host answers them without a fresh document. */
  var READS = {modFiles: true, modFileText: true};
  /* What each file of a mod is to the game, as one phrase (the backend's "class"). */
  var FILE_CLASSES = {
    descriptor: 'About the mod: its name, version, requirements and Toybox text',
    add: 'New file',
    borrow: "A game file under this mod's own name",
    edit: 'Your edit of a game file',
    replace: 'Replaces a game file',
    editor: 'An editor tweak, on this computer only',
    surface: 'A game file SnapMap cannot see on its own',
    '': 'Not served yet'
  };
  var NOT_SERVED_TITLE = 'Snapmap+ takes this file in when the folder changes again, or when you press Rebuild.';
  /* Where a game file's bytes come from: the archive the install bridge matched. */
  var FILE_SOURCES = {
    gameresources: "from the game's campaign and multiplayer files",
    snap_gameresources: "from the game's SnapMap files",
    loose: 'from a file in your install',
    '': 'from your install'
  };
  /* The quiet line above a new mod's example: its three first steps. */
  var NEW_MOD_HINT = "Place Example crate from the Toybox in your map, change this file in the mod's folder, and save it: the mod follows your changes.";
  var NEW_MOD_NOTE = 'Snapmap+ makes a folder for it in mods\\ with an example crate you can place right away.';
  /* What removing a mod from a map severs: wires and timeline references to its placed entities.
     Other reference kinds are not consequences of the removal and are not shown. */
  var REF_KINDS = {wire: 'wire', timeline: 'timeline'};
  var MAP_STATES = {installed: 'ok', active: 'ok', superseded: 'ok'};
  /* A map needs the paths its rows list; a mod folder provides them (the asset rule). A row whose own
     build is not installed reads as not installed, whatever else of its key is: playing the map
     downloads exactly what it pins. */
  var NOT_INSTALLED = 'not installed on this computer; playing the map downloads it';
  var MAP_CAUSES = {missing: NOT_INSTALLED, incomplete: 'installed, but it lacks files this map uses',
                    otherBuild: NOT_INSTALLED, skipped: 'installed but not in use'};
  /* Under the Uninstall confirmation's scope line: what Ctrl+Z and the Recycle Bin can still do. */
  var UNINSTALL_HEAD = {
    undo: 'Undo history is cleared. The folder can be restored from the Recycle Bin by hand. Uninstalling:',
    plain: 'The folder can be restored from the Recycle Bin by hand. Uninstalling:'
  };
  function uninstallHead(pv) { return pv && pv.install && pv.install.undoCleared ? UNINSTALL_HEAD.undo : UNINSTALL_HEAD.plain; }
  /* The saved maps that use the mod (the dry run's savedMaps), and the undo history. */
  function savedMapsText(n) {
    return n + ' of your saved maps use it; they will not open until a mod folder provides its assets again.';
  }
  /* The scope sentence under each confirmation's title, so the two dialogs read differently. */
  var SCOPE = {
    mapRemoveMod: 'Only this map changes. The mod stays installed on this computer and keeps its Toybox rows.',
    uninstallUsed: 'Removes the mod from this computer. The open map is cleaned of it first.',
    uninstallUnused: 'Removes the mod from this computer. The open map does not use it.'
  };
  function confirmScope(pv) { return pv.action === 'uninstall' ? (pv.map ? SCOPE.uninstallUsed : SCOPE.uninstallUnused) : SCOPE.mapRemoveMod; }
  var FILES_NOTE = "Files identical to the game's own are never stored or uploaded; Snapmap+ reads them from your install.";
  var NEEDS_NOTE = 'Every player needs Snapmap+ to play a map that uses this mod.';
  var USE_TITLE = 'Use the selected mod in the whole open map without placing anything; the map lists it when you save';
  var MAP_LEVEL_TEXT = 'Used by the whole map';
  var NOTHING_PLACED_TEXT = 'Declared, nothing placed';
  /* Editor tweaks always apply in this computer's editor, a downloaded map included; a map never
     records them and Play never sees them (DESIGN-additive 2.6, owner 2026-11-09). */
  var EDITOR_TWEAKS_TEXT = 'they apply in your editor whatever map is open and are never saved with a map or used at Play';
  /* A clash names "<game file>/<path>" (modregistry.h), shown as "<path> in <game file>": the
     path starts at the first top-level decl member the slot names. */
  var TOP_MEMBERS = ['edit', 'editorVars', 'inherit', 'events', 'aliases', 'init', 'class'];
  function slotText(slot) {
    var parts = str(slot).split('/'), i;
    for (i = 2; i < parts.length; i++) {
      if (TOP_MEMBERS.indexOf(parts[i]) >= 0) return parts.slice(i).join('/') + ' in ' + parts.slice(0, i).join('/');
    }
    return str(slot);
  }

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
  /* The derived facts (payload facts): null when the backend did not send them. */
  function normFacts(v) {
    var f = obj(v);
    if (!f) return null;
    var reg = obj(f.registries) || {};
    return {adds: num(f.adds), borrows: num(f.borrows), surfaces: num(f.surfaces), editor: num(f.editor), replaces: num(f.replaces),
            editorTweaks: normNames(f.editorTweaks),
            replacements: arr(f.replacements).map(function (r) { r = obj(r) || {}; return {path: str(r.path), edits: bool(r.edits)}; })
              .filter(function (r) { return r.path.length > 0; }),
            edits: arr(f.edits).map(function (e) {
              e = obj(e) || {};
              return {file: str(e.file), ops: num(e.ops), set: num(e.set), remove: num(e.remove), list: num(e.list),
                      keyed: num(e.keyed), bag: num(e.bag), drop: num(e.drop)};
            }).filter(function (e) { return e.file.length > 0; }),
            conductor: normNames(reg.conductor),
            stockBanks: normStockBanks(reg.banks, f.stockBanks)};
  }
  /* registries.banks (the builder's list) with facts.stockBanks (this session's state of each). */
  function normStockBanks(names, states) {
    var byName = {};
    arr(states).forEach(function (s) { s = obj(s) || {}; if (str(s.bank)) byName[str(s.bank)] = s; });
    return normNames(names).map(function (n) {
      var s = byName[n] || {};
      return {bank: n, state: str(s.state) || 'not loaded', ms: num(s.ms), result: num(s.result)};
    });
  }
  function normBuild(v) {
    v = obj(v) || {};
    return {folder: str(v.folder), bundle: str(v.bundle), build: num(v.build), version: str(v.version), builtAt: num(v.builtAt),
            newest: bool(v.newest), failed: bool(v.failed)};
  }
  function normFailed(v) {
    v = obj(v);
    return v ? {cause: str(v.cause), snapshot: bool(v.snapshot), servingBuild: num(v.servingBuild)} : null;
  }
  function normMod(b) {
    b = b && typeof b === 'object' ? b : {};
    var state = has(STATES, b.state) ? b.state : 'attention';
    return {key: str(b.key), id: str(b.id), name: str(b.name), author: str(b.author), version: str(b.version),
            build: num(b.build), size: num(b.size), files: num(b.files), pointers: num(b.pointers), state: state,
            cause: str(b.cause), duplicateId: bool(b.duplicateId),
            editorRows: num(b.editorRows), folder: str(b.folder),
            /* the organizing folder it sits in under mods\ ("" at the root) */
            group: str(b.group),
            /* v3 */
            description: str(b.description), origin: typeof b.origin === 'string' && has(ORIGINS, b.origin) ? b.origin : '',
            migratedFrom: str(b.migratedFrom), servedBy: str(b.servedBy), builtAt: num(b.builtAt), bundle: str(b.bundle),
            renamedFrom: str(b.renamedFrom), renamedReferences: num(b.renamedReferences),
            counts: normCounts(b.counts), cvars: normCvars(b.requirements), commands: normCommands(b.requirements),
            editorRowNames: Array.isArray(b.editorRowNames) ? normNames(b.editorRowNames) : null,
            carriedPaths: Array.isArray(b.carriedPaths) ? normNames(b.carriedPaths) : null,
            publishedMaps: b.publishedMaps === undefined || b.publishedMaps === null ? -1 : num(b.publishedMaps),
            /* additive */
            facts: normFacts(b.facts), newest: b.newest !== false,
            builds: Array.isArray(b.builds) ? b.builds.map(normBuild) : [], buildFailed: normFailed(b.buildFailed),
            /* the builder's warnings (optional): every one, else the first; a warning is not attention */
            warnings: Array.isArray(b.warnings) ? normNames(b.warnings) : (str(b.firstWarning) ? [str(b.firstWarning)] : [])};
  }
  function title(b) { return b.name || b.id || b.key; }
  function cmp(x, y) { return x < y ? -1 : x > y ? 1 : 0; }
  /* The list's one order: the display name without case, then the id, then the folder. */
  function compareMods(a, b) {
    return cmp(title(a).toLowerCase(), title(b).toLowerCase()) || cmp(a.id, b.id) || cmp(a.key, b.key);
  }

  /* The host's payload as the tab uses it. error is set for an unusable payload. */
  function normalize(payload) {
    if (!payload || typeof payload !== 'object' || Array.isArray(payload))
      return {ok: false, error: 'The mods list is unavailable.', mods: [], root: '', gen: 0, v: 0,
              migrate: {entries: 0, pending: 0, running: false}};
    var mg = payload.migrate && typeof payload.migrate === 'object' ? payload.migrate : {};
    var mods = arr(payload.mods).map(normMod);
    mods.sort(compareMods);
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
            references: normRefs(m.references), size: num(m.size), attached: bool(m.attached),
            nothingPlaced: bool(m.nothingPlaced)};
  }
  function normClash(c) {
    c = obj(c) || {};
    return {slot: str(c.slot), chosen: str(c.chosen), serving: str(c.serving),
            providers: arr(c.providers).map(function (p) { p = obj(p) || {}; return {id: str(p.id), name: str(p.name) || str(p.id)}; })
              .filter(function (p) { return p.id.length > 0; })};
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
            vanilla: typeof m.vanilla === 'boolean' ? m.vanilla : null,
            clashes: arr(m.registryClashes).map(normClash).filter(function (c) { return c.slot && c.providers.length > 1; })};
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
            /* Uninstall moves the folder to the Recycle Bin (older backends said move or delete);
               savedMaps is -1 when the saves could not be read. */
            install: ins ? {folder: str(ins.folder), disposition: 'recycle',
                            savedMaps: ins.savedMaps === undefined || ins.savedMaps === null ? -1 : num(ins.savedMaps),
                            undoCleared: bool(ins.undoCleared),
                            /* another build of the key that stays installed: maps open with it */
                            keptBuild: str(ins.keptBuild)} : null,
            /* uninstallPreview only: the Toybox rows uninstalling takes out of the editor */
            editorRows: normNames(v.editorRows)};
  }
  function mapModStatus(r) {
    var tone = MAP_STATES[r.state] || 'warn';
    var cause = tone === 'ok' ? 'Installed' : (MAP_CAUSES[r.state] || r.state);
    return {tone: tone, icon: STATUS[tone].icon, label: STATUS[tone].label, cause: cause};
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
      out.push({text: 'Moves the folder mods\\' + pv.install.folder + ' to the Windows Recycle Bin.'});
      if (pv.install.keptBuild)
        out.push({text: 'Another folder of this mod stays installed (mods\\' + pv.install.keptBuild + '); maps that use the mod open with it.'});
      else if (pv.install.savedMaps > 0) out.push({text: savedMapsText(pv.install.savedMaps)});
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
    if (b.buildFailed) {
      cause = 'its latest build failed: ' + (b.buildFailed.cause || 'see the build log') +
              '; the last working version keeps serving until it builds';
      return {tone: 'warn', icon: STATUS.warn.icon, label: STATUS.warn.label, cause: cause, sentence: 'Needs attention: ' + cause};
    }
    if (b.duplicateId || b.state === 'attention') {
      cause = b.duplicateId ? DUPLICATE_TEXT : (b.cause || 'not in use');
      return {tone: 'warn', icon: STATUS.warn.icon, label: STATUS.warn.label, cause: cause,
              sentence: 'Needs attention: ' + cause};
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

  function originText(b) {
    if (b.origin === 'migration' && b.migratedFrom) return ORIGINS.migration + ' (' + b.migratedFrom + ')';
    return ORIGINS[b.origin] || ORIGINS[''];
  }

  /* A hand-made folder renamed to its generated key at its first build. */
  function renamedText(b) {
    if (!b || !str(b.renamedFrom)) return '';
    return 'Renamed from ' + str(b.renamedFrom) + '; ' + plural(num(b.renamedReferences), 'reference') + ' rewritten';
  }

  function filterMods(mods, query) {
    var q = str(query).trim().toLowerCase();
    if (!q) return mods.slice();
    return mods.filter(function (b) { return (b.name + '\n' + b.id + '\n' + b.key).toLowerCase().indexOf(q) !== -1; });
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

  /* One edited game file: its name and what the edits do. */
  function editLine(e) {
    var parts = [];
    if (e.set) parts.push(plural(e.set, 'field set', 'fields set'));
    if (e.remove) parts.push(plural(e.remove, 'field removed', 'fields removed'));
    if (e.list) parts.push(plural(e.list, 'list entry', 'list entries') + ' added');
    if (e.keyed) parts.push(plural(e.keyed, 'named entry', 'named entries') + ' added or replaced');
    if (e.bag) parts.push(plural(e.bag, 'entry', 'entries') + ' added');
    if (e.drop) parts.push(plural(e.drop, 'entry', 'entries') + ' removed');
    var f = e.file.replace(/^generated\/decls\//, '').replace(/\.decl$/, '');
    return f + ': ' + (parts.length ? parts.join(', ') : 'no change from the game\'s text');
  }

  /* The derived facts as {label, text, list?} lines, in the details pane's order. */
  function factLines(b) {
    var f = b.facts, out = [];
    if (!f) return out;
    if (f.adds) out.push({label: 'Adds', text: plural(f.adds, 'new file')});
    if (f.borrows) out.push({label: 'Borrows', text: plural(f.borrows, 'game file') + ' under its own names, read from your install'});
    if (f.surfaces) out.push({label: 'Surfaces', text: plural(f.surfaces, 'game file') + ' SnapMap cannot see, read from your install'});
    if (f.editor) out.push({label: 'Editor tweaks', text: plural(f.editor, 'file') + ', on this computer only: ' + EDITOR_TWEAKS_TEXT, list: f.editorTweaks});
    if (f.edits.length) {
      out.push({label: 'Edits', text: 'edits ' + plural(f.edits.length, 'game file') + '; maps that declare it get the game\'s text with these edits',
                list: f.edits.map(editLine)});
    }
    /* An edited copy is an edit, never a replacement: facts.replaces and counts.shadows leave it out. */
    var other = f.replacements.filter(function (r) { return !r.edits; });
    if (other.length || f.replaces > 0) {
      out.push({label: 'Replaces', text: plural(Math.max(other.length, f.replaces), 'game file') + ' (a folder the product received)',
                list: other.map(function (r) { return r.path; })});
    }
    var reg = [];
    f.conductor.forEach(function (c) { reg.push('encounter ' + c); });
    if (reg.length) out.push({label: 'Encounters', text: plural(reg.length, 'encounter') + ' listed in the conductor of a map that declares it, its spawn bit assigned per map', list: reg});
    if (f.stockBanks.length) {
      out.push({label: 'Game sounds', text: 'needs ' + f.stockBanks.map(function (s) { return s.bank; }).join(', ') +
                ', single-player sounds SnapMap skips; loaded once per session for a map that declares it',
                list: f.stockBanks.map(function (s) {
                  return s.bank + ': ' + (s.state === 'loaded' && s.ms ? 'loaded (' + s.ms + ' ms)' :
                                          s.state === 'failed' ? 'did not load (AKRESULT ' + s.result + ')' : s.state);
                })});
    }
    return out;
  }

  function actionMessage(action, arg) {
    if (ACTIONS.indexOf(action) < 0) return null;
    var m = {cmd: 'modsAction', action: action};
    if (arg !== undefined && arg !== null && arg !== '') m.arg = str(arg);
    return m;
  }

  /* ---- the Files view: the backend's modFiles answer as the modal uses it */
  function normFile(f) {
    f = obj(f) || {};
    var cls = typeof f['class'] === 'string' && has(FILE_CLASSES, f['class']) ? f['class'] : '';
    var where = str(f.container);
    return {path: str(f.path).replace(/\\/g, '/'), size: num(f.size), text: f.kind === 'text', cls: cls, game: bool(f.game),
            container: has(FILE_SOURCES, where) ? where : '', inFolder: f.inFolder !== false, cause: str(f.cause)};
  }
  function normFiles(d) {
    d = obj(d) || {};
    var files = arr(d.files).map(normFile).filter(function (f) { return f.path.length > 0; });
    files.sort(function (a, b) { return cmp(a.path.toLowerCase(), b.path.toLowerCase()); });
    return {key: str(d.key), name: str(d.name), cause: str(d.cause), files: files, truncated: bool(d.truncated)};
  }
  function fileLeaf(path) { var s = str(path); return s.slice(s.lastIndexOf('/') + 1); }
  /* A game file the mod carries unchanged: read-only, shown muted with the game glyph. */
  function gameFile(f) { return f.game && f.cls !== 'edit'; }
  /* The file's phrases under its path: what it is, where a game file comes from, the size of what has no text. */
  function fileMeta(f) {
    var out = [FILE_CLASSES[f.cls]];
    if (f.game) out.push(FILE_SOURCES[f.container]);
    if (!f.text || !f.inFolder) out.push(formatSize(f.size));
    return out;
  }
  function gameTitle(f) {
    return "The game's own file, " + FILE_SOURCES[f.container] + '. The mod does not carry it.';
  }
  /* The folder tree of the files the filter keeps: folders with their file counts and
     problem counts; a folder holding only one folder shows as one row ("cm/models/x",
     "mods/<key>/demons"). */
  function fileTree(files, query) {
    var q = str(query).trim().toLowerCase();
    function dir(name, path) { return {name: name, path: path, dirs: [], files: [], byName: {}, count: 0, attn: 0}; }
    var root = dir('', '');
    files.forEach(function (f) {
      if (q && f.path.toLowerCase().indexOf(q) < 0) return;
      var parts = f.path.split('/'), node = root, i;
      for (i = 0; i < parts.length - 1; i++) {
        var k = parts[i].toLowerCase();
        if (!has(node.byName, k)) {
          node.byName[k] = dir(parts[i], (node.path ? node.path + '/' : '') + parts[i]);
          node.dirs.push(node.byName[k]);
        }
        node = node.byName[k];
      }
      node.files.push(f);
    });
    function finish(n) {
      n.dirs.forEach(finish);
      n.dirs.sort(function (a, b) { return cmp(a.name.toLowerCase(), b.name.toLowerCase()); });
      n.files.sort(function (a, b) { return cmp(fileLeaf(a.path).toLowerCase(), fileLeaf(b.path).toLowerCase()); });
      n.count = n.files.length;
      n.attn = n.files.filter(function (f) { return !!f.cause; }).length;
      n.dirs.forEach(function (d) { n.count += d.count; n.attn += d.attn; });
      /* A folder above mods/ (a decl type, cooked/model) keeps its own row: it says what the files are. */
      n.dirs = n.dirs.map(function (d) {
        while (!d.files.length && d.dirs.length === 1 && !/^mods(\/|$)/i.test(d.dirs[0].name)) {
          var c = d.dirs[0];
          c.name = d.name + '/' + c.name;
          d = c;
        }
        return d;
      });
    }
    finish(root);
    return root;
  }

  /* ------------------------------------------------------------------------------------------ DOM */

  /* env: {document, byId(id), post(message), toast(text, kind), tokens(text)?, menu(x, y, items)?}.
     tokens returns the decl language's tokens for the Files view's highlighting; menu shows the
     page's context menu. The retired collapsed-groups setting (mods.collapsed_groups) is neither
     read nor written: which organizing folders are closed lasts for the session only. */
  function create(env) {
    var doc = env.document, byId = env.byId;
    var model = normalize(null), loaded = false, waiting = false, busy = '', quiet = false, selectedKey = '';
    var view = [], filter = '', attnOnly = false, lastError = '', rowNodes = {}, closedGroups = {};
    /* The Files modal: the mod it shows, the last modFiles answer, the selected file and its text,
       the filter, the folders opened or closed by hand, and the file and hint a new mod opens on. */
    var fm = {open: false, key: '', data: null, loading: false, error: '', sel: '', text: null, asked: '', filter: '',
              dirs: {}, want: '', hint: '', opener: null, closeBtn: null, list: null, pane: null, visible: [],
              titleNode: null, stateNode: null};
    var reveal = '';   /* a mod the last action made or installed: its organizing folder opens */
    /* The map modal: open, which page ('list' or 'confirm'), the dry run the confirmation shows,
       what opened the confirmation ('map' or 'uninstall') and the element focus returns to. */
    var modal = {open: false, page: 'list', preview: null, from: 'map', opener: null}, modalButtons = [];

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
    var BUSY_TEXT = {install: 'Installing...', rescan: 'Reading the mods folder...',
                     rebuild: 'Building the mod again...', verify: 'Checking every file of the mod...',
                     mapRemoveModPreview: 'Reading what removing it changes...', uninstallPreview: 'Reading what uninstalling it changes...',
                     mapRemoveMod: 'Removing the mod from the map...', uninstall: 'Uninstalling...',
                     mapAddMod: 'Adding the mod to the map...', chooseRegistry: 'Saving the choice...',
                     newMod: 'Making the new mod...',
                     migrate: 'Migrating the old overrides folder; this can take a minute...'};

    function selectedMod() {
      for (var i = 0; i < model.mods.length; i++) if (model.mods[i].key === selectedKey) return model.mods[i];
      return null;
    }
    function attentionCount() { return model.mods.filter(needsAttention).length; }

    function renderButtons() {
      var sel = !!selectedMod();
      ['modsInstallBtn', 'modsFolderBtn', 'modsMigrateBtn', 'modsRefreshBtn', 'modsUninstallBtn', 'modsRebuildBtn', 'modsVerifyBtn'].forEach(function (id) {
        var b = byId(id);
        if (b) b.disabled = !!busy || (!sel && (id === 'modsUninstallBtn' || id === 'modsRebuildBtn' || id === 'modsVerifyBtn'));
      });
      var fb = byId('modsFilesBtn');
      if (fb) fb.disabled = !sel;
      var mb = byId('modsMapBtn');
      if (mb) {
        var mm = model.map && model.map.open && model.map.mods ? model.map.mods.length : -1;
        mb.disabled = !loaded;
        mb.title = mm >= 0 ? 'This map: its details and the ' + plural(mm, 'mod') + ' it uses' : 'This map: its details and the mods it uses';
        mb.setAttribute('aria-label', mb.title);
      }
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

    /* ---- the list: plain rows in the model's order; every row shown is mounted. A mod in an
       organizing folder of mods\ sits under that folder's row (the Prefabs tab's caret and count),
       folders first; a closed folder hides its rows for the session. view is the rows shown, in
       the order the arrow keys walk. */
    function computeView() {
      var shown = filterMods(model.mods, filter);
      if (attnOnly) shown = shown.filter(needsAttention);
      view = shown;
      return shown;
    }
    function groupClosed(g) { return closedGroups[g] === true && !str(filter).trim() && !attnOnly; }
    function groupRow(g, members) {
      var closed = groupClosed(g);
      var f = el('div', 'pf-folder mods-folder' + (closed ? '' : ' pf-open'));
      f.setAttribute('data-group', g);
      var head = el('div', 'pf-folder-head');
      head.setAttribute('role', 'button');
      head.setAttribute('aria-expanded', closed ? 'false' : 'true');
      head.title = 'mods\\' + g;
      var caret = el('span', 'pf-caret');
      caret.appendChild(icon('chevron-down'));
      head.appendChild(caret);
      head.appendChild(el('span', 'pf-fname', g.replace(/\\/g, '/')));
      head.appendChild(el('span', 'pf-fcount', String(members.length)));
      var attn = members.filter(needsAttention).length;
      if (closed && attn) {
        var w = statusIcon({tone: 'warn', icon: STATUS.warn.icon, label: plural(attn, 'mod needs', 'mods need') + ' attention'});
        w.title = w.getAttribute('aria-label');
        head.appendChild(w);
      }
      head.addEventListener('click', function () { closedGroups[g] = !groupClosed(g); renderList(); });
      f.appendChild(head);
      var body = el('div', 'pf-folder-body');
      if (!closed) members.forEach(function (b) { body.appendChild(modRow(b)); });
      f.appendChild(body);
      return f;
    }
    function newModButton() {
      var b = el('button', 'btn mods-new-btn', 'New mod');
      b.type = 'button';
      b.title = 'Make a mod of your own, with an example to start from';
      b.addEventListener('click', function () { openNewMod(b); });
      return b;
    }
    /* The open map uses the mod: the document's map.mods names its folder. */
    function inMap(b) { return !!(model.map && model.map.open && mapUses(b)); }
    function renderList() {
      var box = byId('modsList');
      if (!box) return;
      clear(box);
      rowNodes = {};
      var shown = computeView();
      if (!loaded) { box.appendChild(el('div', 'list-empty', waiting ? 'Reading installed mods...' : 'Open this tab to list installed mods.')); return; }
      if (lastError) { box.appendChild(el('div', 'list-empty mods-error', lastError)); return; }
      if (!model.mods.length) {
        var card = el('div', 'list-empty mods-empty');
        card.appendChild(el('div', '', 'No mods installed. Drop a mod folder into mods\\ or use the download button to install one.'));
        card.appendChild(el('div', '', 'Or make your own, starting from a working example.'));
        card.appendChild(newModButton());
        box.appendChild(card);
        return;
      }
      if (!shown.length) {
        box.appendChild(el('div', 'list-empty', attnOnly ? 'No mod that needs attention matches the filter.' : 'No installed mod matches the filter.'));
        return;
      }
      var groups = {}, names = [], root = [];
      shown.forEach(function (b) {
        if (!b.group) { root.push(b); return; }
        if (!has(groups, b.group)) { groups[b.group] = []; names.push(b.group); }
        groups[b.group].push(b);
      });
      names.sort(function (x, y) { return cmp(x.toLowerCase(), y.toLowerCase()) || cmp(x, y); });
      view = [];
      names.forEach(function (g) {
        box.appendChild(groupRow(g, groups[g]));
        if (!groupClosed(g)) view = view.concat(groups[g]);
      });
      root.forEach(function (b) { box.appendChild(modRow(b)); view.push(b); });
    }
    function modRow(b) {
      var info = statusInfo(b), on = b.key === selectedKey, used = inMap(b);
      var row = el('div', 'mods-row' + (on ? ' selected' : ''));
      row.setAttribute('role', 'option');
      row.setAttribute('data-key', b.key);
      row.setAttribute('aria-selected', on ? 'true' : 'false');
      if (used) row.setAttribute('data-in-map', 'true');
      /* One glyph: needs attention wins, then in this map, then installed; the cause is the hover. */
      row.title = info.cause + (used ? '. ' + IN_MAP_TITLE + '.' : '');
      row.appendChild(statusIcon(info.tone !== 'warn' && used ? IN_MAP_GLYPH : info));
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
      /* A mod in a closed organizing folder opens its folder. */
      var sm = selectedMod();
      if (sm && sm.group && groupClosed(sm.group)) { closedGroups[sm.group] = false; renderList(); }
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
    /* Arrow keys, Home and End move the selection over the rows shown; Page Up and Page Down
       scroll natively. */
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
      if (b.author) field(about, 'Author', b.author);
      if (b.description) field(about, 'Description', b.description).className += ' mods-field-long';
      var link = el('button', 'mods-link', b.folder || b.key);
      link.type = 'button';
      link.title = 'Open this folder in Explorer';
      link.addEventListener('click', function () { act('openFolder', b.folder || b.key); });
      field(about, 'Folder', link);
      about.appendChild(el('div', 'mods-line mods-origin', originText(b)));
      if (renamedText(b)) about.appendChild(el('div', 'mods-line mods-origin', renamedText(b)));

      var adds = section(box, 'adds', 'What it adds');
      var names = b.editorRowNames || [];
      if (names.length) {
        var list = el('div', 'mods-rows-list');
        names.forEach(function (n) { list.appendChild(el('div', 'mods-rows-item', n)); });
        field(adds, plural(names.length, 'Toybox row'), list);
      } else if (b.editorRows > 0) {
        adds.appendChild(el('div', 'mods-line', plural(b.editorRows, 'Toybox row')));
      }
      /* Editor tweaks change stock rows in this computer's editor; they are not rows the mod adds. */
      if (b.facts && b.facts.editor) adds.appendChild(el('div', 'mods-line', plural(b.facts.editor, 'editor tweak')));
      countLines(b.counts).forEach(function (t) { adds.appendChild(el('div', 'mods-line', t)); });
      if (adds.children.length === 1) adds.appendChild(el('div', 'mods-line muted', 'Adds no Toybox rows.'));

      var facts = factLines(b);
      if (facts.length) {
        var fs = section(box, 'facts', 'What it is made of');
        facts.forEach(function (l) {
          if (l.list && l.list.length) {
            var d = el('details', 'pc-limits-details mods-paths');
            d.appendChild(el('summary', '', l.label + ': ' + l.text));
            var dl = el('div', 'mods-paths-list');
            l.list.forEach(function (n) { dl.appendChild(el('div', '', n)); });
            d.appendChild(dl);
            fs.appendChild(d);
          } else field(fs, l.label, l.text);
        });
      }
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

      var when = formatDate(b.builtAt);
      if (when || b.bundle || b.warnings.length) {
        var bl = section(box, 'build', 'Build');
        if (when) field(bl, 'Last built', when);
        if (b.bundle) {
          var br = field(bl, 'Bundle', b.bundle.slice(0, 16));
          br.title = 'Quote this when you ask for help with this mod';
          br.className += ' mods-bundle';
        }
        b.warnings.forEach(function (w) { bl.appendChild(el('div', 'mods-line mods-warning', w)); });
        if (b.bundle) bl.appendChild(el('div', 'mods-note', 'Quote the bundle number when you ask for help with this mod.'));
      }
      if (b.publishedMaps >= 0) {
        var share = section(box, 'sharing', 'Sharing');
        field(share, 'Published maps', b.publishedMaps ? 'Used by ' + plural(b.publishedMaps, 'of your published maps', 'of your published maps') : 'Used by none of your published maps');
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
      if (m.clashes.length) {
        var cs = section(body, 'clashes', 'Choices between mods');
        cs.appendChild(el('div', 'mods-note', 'Several mods of this map edit the same field of a game file differently. Pick whose edit this map uses; it applies now and is saved with the map\'s next save.'));
        m.clashes.forEach(function (c) {
          var sel = el('select', 'mods-clash');
          sel.setAttribute('data-slot', c.slot);
          c.providers.forEach(function (p) {
            var o = el('option', '', p.name + (p.name !== p.id ? ' (' + p.id + ')' : ''));
            o.value = p.id;
            if (p.id === c.serving) o.selected = true;
            sel.appendChild(o);
          });
          sel.value = c.serving;
          sel.disabled = !!busy;
          sel.addEventListener('change', function () { act('chooseRegistry', c.slot + '\t' + str(sel.value)); });
          modalButtons.push(sel);
          field(cs, slotText(c.slot), sel);
        });
      }
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
      var uses = r.nothingPlaced ? NOTHING_PLACED_TEXT : r.attached
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
        ? uninstallHead(pv)
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

    /* ---- the Files modal (read-only): the standard modal at the expanded Decl Text editor's size.
       Left, the mod folder as a tree with a filter; right, the selected file: its path once, what
       it is, then its text in the Decl Text editor primitive (read-only), and the cause that
       names it in the Entity State problem strip. The two reads (modFiles, modFileText) go to the
       host without the page's busy state: they change nothing and no fresh document follows. */
    function filesOverlay(on) {
      var ov = byId('modsFilesModal');
      if (!ov) return;
      ov.style.display = on ? 'flex' : 'none';
      ov.setAttribute('aria-hidden', on ? 'false' : 'true');
    }
    function read(action, arg) {
      var m = actionMessage(action, arg);
      if (m) env.post(m);
    }
    function openFiles(key, opener, want, hint) {
      if (!key) return;
      fm.open = true;
      fm.key = key;
      fm.data = null;
      fm.loading = true;
      fm.error = '';
      fm.sel = '';
      fm.text = null;
      fm.asked = '';
      fm.filter = '';
      fm.dirs = {};
      fm.want = str(want);
      fm.hint = str(hint);
      if (opener !== undefined) fm.opener = opener;
      filesOverlay(true);
      renderFiles();
      if (fm.closeBtn && fm.closeBtn.focus) fm.closeBtn.focus();
      read('modFiles', key);
    }
    function closeFiles() {
      if (!fm.open) return;
      fm.open = false;
      filesOverlay(false);
      clear(byId('modsFilesHost'));
      fm.list = fm.pane = fm.closeBtn = fm.titleNode = fm.stateNode = null;
      if (fm.opener && fm.opener.focus) fm.opener.focus();
      fm.opener = null;
    }
    function fileOf(path) {
      var files = fm.data ? fm.data.files : [];
      for (var i = 0; i < files.length; i++) if (files[i].path === path) return files[i];
      return null;
    }
    function selectFile(path) {
      var f = fileOf(path);
      fm.sel = path;
      fm.text = null;
      fm.asked = '';
      if (f && f.text && f.inFolder) {
        fm.asked = path;
        fm.text = {loading: true};
        read('modFileText', fm.key + '\t' + path);
      }
      renderFileTree();
      renderFilePane();
    }
    function filesTitle() {
      var b = null;
      for (var i = 0; i < model.mods.length; i++) if (model.mods[i].key === fm.key) b = model.mods[i];
      return {name: b ? title(b) : (fm.data && fm.data.name) || fm.key, info: b ? statusInfo(b) : null};
    }
    /* The title and the one-line state follow each document (a rebuild, a pushed change). */
    function renderFilesTitle() {
      if (!fm.open || !fm.titleNode) return;
      var t = filesTitle();
      fm.titleNode.textContent = t.name;
      fm.stateNode.textContent = t.info ? t.info.sentence : '';
      fm.stateNode.className = 'mf-state' + (t.info && t.info.tone === 'warn' ? ' warn' : '');
    }
    function renderFiles() {
      var host = byId('modsFilesHost');
      if (!host || !fm.open) return;
      clear(host);
      var t = filesTitle();
      var head = el('div', 'mods-map-head mf-head');
      var name = el('div', 'modal-title', t.name);
      name.id = 'modsFilesTitle';
      head.appendChild(name);
      fm.titleNode = name;
      fm.stateNode = el('span', 'mf-state');
      head.appendChild(fm.stateNode);
      renderFilesTitle();
      var x = el('button', 'btn icon icon-button mods-map-close');
      x.type = 'button';
      x.title = 'Close (Esc)';
      x.setAttribute('aria-label', 'Close');
      x.appendChild(icon('x'));
      x.addEventListener('click', closeFiles);
      head.appendChild(x);
      fm.closeBtn = x;
      host.appendChild(head);
      var body = el('div', 'mf-body');
      var tree = el('div', 'mf-tree');
      var input = el('input', 'mf-filter');
      input.type = 'text';
      input.value = fm.filter;
      input.setAttribute('placeholder', 'Filter files...');
      input.setAttribute('aria-label', 'Filter files');
      input.addEventListener('input', function () { fm.filter = str(input.value); renderFileTree(); });
      tree.appendChild(input);
      fm.list = el('div', 'mf-list list');
      fm.list.setAttribute('role', 'tree');
      fm.list.setAttribute('aria-label', 'Files of ' + t.name);
      fm.list.addEventListener('keydown', onFilesListKey);
      tree.appendChild(fm.list);
      body.appendChild(tree);
      fm.pane = el('div', 'mf-file');
      body.appendChild(fm.pane);
      host.appendChild(body);
      renderFileTree();
      renderFilePane();
    }
    /* Folders on the selected file's path, and the first level, start open; a filter opens all. */
    function dirOpen(d, depth) {
      if (str(fm.filter).trim()) return true;
      if (has(fm.dirs, d.path)) return fm.dirs[d.path];
      return depth === 0 || (!!fm.sel && fm.sel.toLowerCase().indexOf(d.path.toLowerCase() + '/') === 0);
    }
    function fileGlyph(cls, name, label) {
      var g = el('span', 'mf-g' + (cls ? ' ' + cls : ''));
      g.setAttribute('role', 'img');
      g.setAttribute('aria-label', label);
      g.title = label;
      g.appendChild(icon(name));
      return g;
    }
    function fileRow(f) {
      var on = f.path === fm.sel;
      var r = el('div', 'pf-item mf-item' + (gameFile(f) ? ' mf-ro' : '') + (on ? ' selected' : ''));
      r.setAttribute('role', 'treeitem');
      r.setAttribute('data-path', f.path);
      r.setAttribute('aria-selected', on ? 'true' : 'false');
      r.setAttribute('tabindex', on ? '0' : '-1');
      r.title = f.path;
      r.appendChild(el('span', 'mf-name', fileLeaf(f.path)));
      /* One glyph at most: a problem, else the game's own file; the mod's own files have none. */
      if (f.cause) r.appendChild(fileGlyph('attn', 'triangle-alert', f.cause));
      else if (gameFile(f)) r.appendChild(fileGlyph('', 'gamepad-2', gameTitle(f)));
      r.addEventListener('click', function () { selectFile(f.path); });
      fm.visible.push(f.path);
      return r;
    }
    function treeInto(box, node, depth) {
      node.files.forEach(function (f) { box.appendChild(fileRow(f)); });
      node.dirs.forEach(function (d) {
        var open = dirOpen(d, depth);
        var fo = el('div', 'pf-folder' + (open ? ' pf-open' : ''));
        var head = el('div', 'pf-folder-head');
        head.setAttribute('role', 'treeitem');
        head.setAttribute('aria-expanded', open ? 'true' : 'false');
        head.title = d.path;
        var caret = el('span', 'pf-caret');
        caret.appendChild(icon('chevron-down'));
        head.appendChild(caret);
        head.appendChild(el('span', 'pf-fname', d.name));
        head.appendChild(el('span', 'pf-fcount', String(d.count)));
        if (!open && d.attn) head.appendChild(fileGlyph('attn', 'triangle-alert', 'A file in this folder has a problem'));
        head.addEventListener('click', function () { fm.dirs[d.path] = !open; renderFileTree(); });
        fo.appendChild(head);
        var body = el('div', 'pf-folder-body');
        if (open) treeInto(body, d, depth + 1);
        fo.appendChild(body);
        box.appendChild(fo);
      });
    }
    function renderFileTree() {
      var box = fm.list;
      if (!box) return;
      clear(box);
      fm.visible = [];
      if (fm.loading) { box.appendChild(el('div', 'list-empty', 'Reading the files...')); return; }
      if (fm.error) { box.appendChild(el('div', 'list-empty mods-error', fm.error)); return; }
      var root = fileTree(fm.data.files, fm.filter);
      if (!root.count) { box.appendChild(el('div', 'list-empty', fm.data.files.length ? 'No file matches the filter.' : 'The folder holds no files.')); return; }
      treeInto(box, root, 0);
      if (fm.data.truncated) box.appendChild(el('div', 'list-empty', 'The folder holds more files than the view lists.'));
    }
    /* Arrow keys walk the files shown. */
    function onFilesListKey(e) {
      var step = e.key === 'ArrowDown' ? 1 : e.key === 'ArrowUp' ? -1 : 0;
      if (!step || !fm.visible.length) return;
      if (e.preventDefault) e.preventDefault();
      var at = fm.visible.indexOf(fm.sel);
      selectFile(fm.visible[Math.max(0, Math.min(fm.visible.length - 1, at < 0 ? 0 : at + step))]);
    }
    /* The Decl Text editor primitive, read-only: line numbers, the highlighted text, and a
       read-only textarea above it for selection and copy. */
    function codeEditor(text, label) {
      var t = str(text).replace(/\r\n?/g, '\n');
      var ed = el('div', 'code-editor mf-editor');
      /* a final line break ends the last line; it does not start another */
      var lines = Math.max(1, t.split('\n').length - (/\n$/.test(t) ? 1 : 0)), digits = Math.max(2, String(lines).length), nums = [];
      for (var i = 1; i <= lines; i++) nums.push(i);
      if (ed.style && ed.style.setProperty) ed.style.setProperty('--ceGutterW', (Math.ceil(digits * 6.9) + 12) + 'px');
      var gut = el('div', 'ce-gutter');
      gut.setAttribute('aria-hidden', 'true');
      var gl = el('div', '', nums.join('\n'));
      gut.appendChild(gl);
      ed.appendChild(gut);
      var pre = el('pre');
      pre.setAttribute('aria-hidden', 'true');
      var code = el('code');
      var toks = null;
      try { toks = env.tokens ? env.tokens(t) : null; } catch (err) { toks = null; }
      if (toks && toks.length) {
        var last = 0;
        toks.forEach(function (tk) {
          if (tk.start > last) code.appendChild(el('span', '', t.slice(last, tk.start)));
          var cls = tk.type === 'cmt' ? 'tk-cmt' : tk.type === 'str' ? 'tk-str' : tk.type === 'num' ? 'tk-num'
                  : tk.isBool ? 'tk-bool' : tk.isKey ? 'tk-key' : tk.type === 'ident' ? 'tk-ident' : tk.type === 'bad' ? 'tk-bad' : 'tk-punc';
          code.appendChild(el('span', cls, t.slice(tk.start, tk.end)));
          last = tk.end;
        });
        code.appendChild(el('span', '', t.slice(last) + '\n'));
      } else code.textContent = t + '\n';
      pre.appendChild(code);
      ed.appendChild(pre);
      var ta = el('textarea');
      ta.readOnly = true;
      ta.setAttribute('readonly', 'readonly');
      ta.setAttribute('spellcheck', 'false');
      ta.setAttribute('wrap', 'off');
      ta.setAttribute('aria-label', label);
      ta.value = t;
      ta.addEventListener('scroll', function () {
        pre.scrollTop = ta.scrollTop;
        pre.scrollLeft = ta.scrollLeft;
        gl.style.transform = 'translateY(' + (-ta.scrollTop) + 'px)';
      });
      ed.appendChild(ta);
      return ed;
    }
    function problemStrip(box, cause) {
      if (!cause) return;
      var bar = el('div', 'problems has mf-problems');
      bar.setAttribute('role', 'list');
      var p = el('div', 'prob');
      p.setAttribute('role', 'listitem');
      var sev = el('span', 'sev err');
      sev.appendChild(icon('triangle-alert'));
      p.appendChild(sev);
      p.appendChild(el('span', '', cause));
      bar.appendChild(p);
      box.appendChild(bar);
    }
    function renderFilePane() {
      var pane = fm.pane;
      if (!pane) return;
      clear(pane);
      if (fm.loading || fm.error) return;
      var f = fileOf(fm.sel);
      if (!f) { pane.appendChild(el('div', 'mf-binary', 'Select a file to see it.')); return; }
      var head = el('div', 'mf-fhead');
      var cut = f.path.lastIndexOf('/') + 1;
      var path = el('div', 'mf-path');
      if (cut > 0) path.appendChild(el('span', 'dir', f.path.slice(0, cut)));
      path.appendChild(el('span', '', f.path.slice(cut)));
      head.appendChild(path);
      var meta = el('div', 'mf-meta');
      fileMeta(f).forEach(function (m, i) {
        var s = el('span', '', m);
        if (i === 0 && !f.cls) s.title = NOT_SERVED_TITLE;
        meta.appendChild(s);
      });
      head.appendChild(meta);
      pane.appendChild(head);
      if (fm.hint && f.path === fm.want) pane.appendChild(el('div', 'mf-hint', fm.hint));
      var tx = fm.text && fm.asked === f.path ? fm.text : null;
      if (!f.text) pane.appendChild(el('div', 'mf-binary', 'A binary file: it has no text to show here.'));
      else if (!f.inFolder) pane.appendChild(el('div', 'mf-binary', "The mod's folder does not hold this file; Snapmap+ reads it from your install."));
      else if (!tx || tx.loading) pane.appendChild(el('div', 'mf-binary', 'Reading the file...'));
      else if (tx.error) pane.appendChild(el('div', 'mf-binary mods-error', tx.error));
      else {
        if (tx.productBlock) pane.appendChild(el('div', 'mf-hint', 'Snapmap+ writes the rest of mod.json itself; that part is not shown.'));
        pane.appendChild(codeEditor(tx.text, 'Text of ' + fileLeaf(f.path)));
      }
      problemStrip(pane, (tx && tx.cause) || f.cause);
    }
    function onFilesResult(action, ok, message, data) {
      if (!fm.open) return;
      if (action === 'modFiles') {
        if (data && str(data.key) && str(data.key) !== fm.key) return;
        fm.loading = false;
        if (!ok) { fm.error = message || 'The files could not be read.'; renderFiles(); return; }
        fm.data = normFiles(data);
        var pick = fileOf(fm.want) ? fm.want : '';
        fm.data.files.forEach(function (f) { if (!pick && f.cause) pick = f.path; });
        if (!pick && fileOf('mod.json')) pick = 'mod.json';
        if (!pick && fm.data.files.length) pick = fm.data.files[0].path;
        renderFiles();
        if (pick) selectFile(pick);
        return;
      }
      /* modFileText: a refusal does not name its file, so it answers the file last asked for. */
      if (!fm.asked || (data && str(data.path) && str(data.path) !== fm.asked)) return;
      fm.text = ok ? {text: str(data && data.text), productBlock: bool(data && data.productBlock), cause: str(data && data.cause)}
                   : {error: message || 'The file could not be read.'};
      renderFilePane();
    }
    function onFilesKey(e) {
      if (!fm.open) return;
      if (e.key === 'Escape') { if (e.preventDefault) e.preventDefault(); closeFiles(); return; }
      trapTab(e, byId('modsFilesHost'));
    }

    /* ---- New mod: a name, then the backend makes the folder and its example; the list selects
       it and the Files modal opens on the example's Toybox row. */
    var nm = {opener: null};
    function openNewMod(opener) {
      var ov = byId('modsNewModal'), input = byId('modsNewName');
      if (!ov || busy) return;
      nm.opener = opener || null;
      if (input) input.value = '';
      var note = byId('modsNewNote');
      if (note) note.textContent = NEW_MOD_NOTE;
      ov.style.display = 'flex';
      ov.setAttribute('aria-hidden', 'false');
      if (input && input.focus) input.focus();
    }
    function closeNewMod() {
      var ov = byId('modsNewModal');
      if (!ov || ov.style.display === 'none') return;
      ov.style.display = 'none';
      ov.setAttribute('aria-hidden', 'true');
      if (nm.opener && nm.opener.focus) nm.opener.focus();
      nm.opener = null;
    }
    function createNewMod() {
      var input = byId('modsNewName'), name = str(input && input.value).trim();
      if (!name) { env.toast('Give the new mod a name.', 'warn'); if (input && input.focus) input.focus(); return; }
      if (act('newMod', name)) closeNewMod();
    }
    function onNewKey(e) {
      if (e.key === 'Escape') { if (e.preventDefault) e.preventDefault(); closeNewMod(); }
      else if (e.key === 'Enter') { if (e.preventDefault) e.preventDefault(); createNewMod(); }
      else trapTab(e, byId('modsNewModal'));
    }
    function trapTab(e, host) {
      if (e.key !== 'Tab' || !host || !host.querySelectorAll) return;
      var f = Array.prototype.filter.call(host.querySelectorAll('button, input, textarea, summary, [tabindex="0"]'),
        function (n) { return !n.disabled && n.offsetParent !== null; });
      if (!f.length) return;
      var first = f[0], last = f[f.length - 1], at = doc.activeElement;
      if (e.shiftKey && (at === first || !host.contains(at))) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && (at === last || !host.contains(at))) { e.preventDefault(); first.focus(); }
    }

    function renderHead() {
      var badge = byId('modsBadge');
      if (badge) badge.textContent = String(model.mods.length);
      var n = attentionCount(), at = byId('modsAttnBtn');
      if (n === 0 && attnOnly) attnOnly = false;
      if (at) {
        clear(at);
        at.appendChild(icon('triangle-alert'));
        at.style.display = loaded && n > 0 ? '' : 'none';
        at.setAttribute('aria-pressed', attnOnly ? 'true' : 'false');
        at.className = 'btn icon icon-button mods-attn';
        at.title = plural(n, 'mod needs', 'mods need') + ' attention: ' + (attnOnly ? 'show every mod' : 'show only these');
        at.setAttribute('aria-label', at.title);
      }
      var rootLine = byId('modsRoot');
      if (rootLine) { rootLine.textContent = model.root; rootLine.title = model.root; }
    }

    function renderAll() {
      if (selectedKey && !selectedMod()) selectedKey = '';
      if (reveal && selectedMod() && selectedKey === reveal) {
        if (selectedMod().group) closedGroups[selectedMod().group] = false;
        reveal = '';
      }
      renderFilesTitle();
      renderHead();
      renderList();
      renderDetail();
      renderModal();
      renderButtons();
    }
    function setAttnOnly(on) {
      attnOnly = !!on && attentionCount() > 0;
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
        /* A pushed document (a console verb changed the store or the open map) keeps the status line. */
        if (!busy && !d.pushed) setStatus('', '');
      }
      renderAll();
    }
    function onResult(d) {
      var action = str(d.action), data = d.data && typeof d.data === 'object' ? d.data : {};
      var message = str(d.message || data.message), ok = d.ok === true || data.ok === true;
      /* The Files view's reads: answered to the modal only, never to the busy state or the status line. */
      if (has(READS, action)) { onFilesResult(action, ok, message, data); return true; }
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
      if (action === 'install' && ok && data.key) reveal = selectedKey = str(data.key);
      /* A new mod: selected, and the Files modal opens on its example's Toybox row. */
      if (action === 'newMod' && ok && data.key) {
        reveal = selectedKey = str(data.key);
        openFiles(selectedKey, byId('modsFilesBtn'), str(data.file), NEW_MOD_HINT);
      }
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
      on('modsFilesBtn', 'click', function () { var b = selectedMod(); if (b) openFiles(b.key, byId('modsFilesBtn')); });
      on('modsFilesModal', 'keydown', onFilesKey);
      on('modsFilesModal', 'mousedown', function (e) { if (e.target === byId('modsFilesModal')) closeFiles(); });
      on('modsNewModal', 'keydown', onNewKey);
      on('modsNewModal', 'mousedown', function (e) { if (e.target === byId('modsNewModal')) closeNewMod(); });
      on('modsNewCancel', 'click', closeNewMod);
      on('modsNewCreate', 'click', createNewMod);
      /* New mod has no room in the list's header at its minimum width (six icon buttons fill it):
         it is the empty list's button and the list's right-click entry. */
      on('modsList', 'contextmenu', function (e) {
        if (!env.menu) return;
        if (e.preventDefault) e.preventDefault();
        env.menu(e.pageX, e.pageY, [{label: 'New mod...', run: function () { openNewMod(byId('modsList')); }}]);
      });
      on('modsAttnBtn', 'click', function () { setAttnOnly(!attnOnly); });
      on('modsFilter', 'input', function () { filter = str(byId('modsFilter').value); renderList(); });
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
      closeMap: closeMap,
      openFiles: function (key, want, hint) { openFiles(key, undefined, want, hint); },
      closeFiles: closeFiles,
      selectFile: selectFile,
      openNewMod: function () { openNewMod(); },
      onMessage: function (d) {
        if (!d || typeof d !== 'object') return false;
        if (d.kind === 'mods') { onMods(d); return true; }
        if (d.kind === 'modsActionResult') { onResult(d); return true; }
        return false;
      },
      state: function () {
        return {model: model, loaded: loaded, waiting: waiting, busy: busy, selectedKey: selectedKey, view: view, error: lastError,
                attentionOnly: attnOnly, closedGroups: Object.keys(closedGroups).filter(function (g) { return closedGroups[g]; }),
                modal: {open: modal.open, page: modal.page, from: modal.from, preview: modal.preview},
                files: {open: fm.open, key: fm.key, loading: fm.loading, error: fm.error, data: fm.data, selected: fm.sel,
                        text: fm.text, want: fm.want, hint: fm.hint}};
      }
    };
  }

  /* ------------------------------------------------------------------------------- stand-in host */

  /* The browser preview's backend: it holds a copy of a document, answers each modsAction as the
     host does (the action's result, then a fresh document) and changes its copy the way the backend
     would, so the page is exercised by the same render-from-the-document path as in the game.
     handle(message) returns the messages the host would post. Used by mockup.html and the tests. */
  /* folders (optional): {key: {files: [modFiles entries], texts: {path: text}}}, the preview's
     files of some mods; any other mod lists mod.json and its carried paths. */
  function standIn(initial, folders) {
    var docu = JSON.parse(JSON.stringify(initial || {}));
    var disk = JSON.parse(JSON.stringify(folders || {}));
    function fileEntry(path, size, cls, extra) {
      var f = {path: path, size: size, kind: /\.(decl|json|txt|md|cfg|entities)$/i.test(path) ? 'text' : 'binary', 'class': cls,
               game: false, container: '', inFolder: true, cause: ''};
      Object.keys(extra || {}).forEach(function (k) { f[k] = extra[k]; });
      return f;
    }
    function folderOf(b) {
      if (has(disk, b.key)) return disk[b.key];
      var files = [fileEntry('mod.json', 400, 'descriptor')];
      arr(b.carriedPaths).forEach(function (p) { files.push(fileEntry('assets/' + p, 0, 'add')); });
      return {files: files, texts: {}};
    }
    /* New mod's example, as the backend writes it (modstore.c ms_write_example). */
    function exampleFolder(key, name) {
      var def = 'assets/generated/decls/entitydef/mods/' + key + '/example_crate.decl';
      var row = 'assets/generated/decls/snapeditorentitydef/mods/' + key + '/example_crate.decl';
      var texts = {};
      texts['mod.json'] = '{\r\n  "name": ' + JSON.stringify(name) + ',\r\n  "version": "0.1.0",\r\n  "description": "",\r\n  "author": "",\r\n' +
        '  "strings": {\r\n    "en": {\r\n      "' + key + '_category": ' + JSON.stringify(name) + ',\r\n' +
        '      "' + key + '_example_crate_name": "Example crate",\r\n' +
        '      "' + key + '_example_crate_desc": "A crate your new mod adds. Change it in the mod\'s files."\r\n    }\r\n  }\r\n}\r\n';
      texts[def] = '{\r\n\tclass = "idProp_Coop";\r\n\tedit = {\r\n\t\trenderModelInfo = {\r\n\t\t\tmodel = "models/snapmaps/props/new/crate01.lwo";\r\n' +
        '\t\t}\r\n\t\tclipModelInfo = {\r\n\t\t\ttype = "CLIPMODEL_AUTO";\r\n\t\t}\r\n\t\tdynamicObstacle = true;\r\n\t}\r\n}\r\n';
      texts[row] = '{\r\n\tinherit = "prop/prop_base";\r\n\tedit = {\r\n' +
        '\t\tpreviewImage = "textures/guis/snapmaps/entity_icons/prop/crate01";\r\n' +
        '\t\tmetaCategoryNameTag = "#str_snapentity_metacategory_toybox";\r\n' +
        '\t\tcategoryNameTag = "#str_' + key + '_category";\r\n' +
        '\t\tdisplayNameTag = "#str_' + key + '_example_crate_name";\r\n' +
        '\t\tdisplayDescriptionTag = "#str_' + key + '_example_crate_desc";\r\n' +
        '\t\tentityDef = "mods/' + key + '/example_crate";\r\n' +
        '\t\trenderInfo = {\r\n\t\t\trenderModelName = "models/snapmaps/props/new/crate01.lwo";\r\n\t\t\talwaysDrawBounds = false;\r\n' +
        '\t\t\tsize = {\r\n\t\t\t\tx = 54;\r\n\t\t\t\ty = 54;\r\n\t\t\t\tz = 54;\r\n\t\t\t}\r\n\t\t}\r\n' +
        '\t\tflags = {\r\n\t\t\tplaceable = true;\r\n\t\t}\r\n\t}\r\n}\r\n';
      return {row: row, folder: {files: [fileEntry('mod.json', texts['mod.json'].length, 'descriptor'),
                                          fileEntry(def, texts[def].length, 'add'), fileEntry(row, texts[row].length, 'add')], texts: texts}};
    }
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
    /* Uninstall moves the folder to the Recycle Bin; the saved maps are not the preview's to count. */
    function installPart(b, open) {
      return {folder: b.folder || b.key, disposition: 'recycle', savedMaps: num(b.savedMaps), undoCleared: !!open};
    }
    function removeFromMap(r) {
      docu.map.mods = mapMods().filter(function (x) { return x !== r; });
      docu.map.entities = Math.max(0, num(docu.map.entities) - num(r.placedCount));
    }
    return function handle(o) {
      var a = str(o && o.action), key = str(o && o.arg), ok = true, msg = '', data = {action: a};
      if (a === 'openFolder') return [];
      if (has(READS, a)) {
        /* The Files view's reads: the result alone, as the host answers them. */
        var tab = key.indexOf('\t'), mk = tab < 0 ? key : key.slice(0, tab), fp = tab < 0 ? '' : key.slice(tab + 1);
        var mb = modOf(mk), fo = mb ? folderOf(mb) : null;
        data.key = mk;
        if (!mb) { ok = false; msg = 'That mod is no longer installed.'; }
        else if (a === 'modFiles') {
          data.name = str(mb.name);
          data.cause = str(mb.cause);
          data.files = JSON.parse(JSON.stringify(fo.files));
          data.truncated = false;
        } else {
          var ent = fo.files.filter(function (f) { return f.path === fp; })[0];
          if (!ent) { ok = false; msg = "The mod's folder does not hold this file; it is read from your install."; }
          else if (ent.kind !== 'text') { ok = false; msg = 'This file is binary; it has no text to show.'; }
          else if (!ent.inFolder) { ok = false; msg = "The mod's folder does not hold this file; it is read from your install."; }
          else {
            data.path = fp;
            data.text = has(fo.texts, fp) ? fo.texts[fp] : '// Browser preview: in the game this is the file\'s text from the mods folder.\r\n';
            data.size = data.text.length;
            data.productBlock = fp === 'mod.json';
            data.cause = str(ent.cause);
          }
        }
        data.ok = ok;
        data.message = msg;
        return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}];
      }
      if (a === 'newMod') {
        var nmName = key.trim(), slug = nmName.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '') || 'mod';
        if (!nmName) { ok = false; msg = 'No mod was created: a new mod needs a name.'; }
        else {
          var nk = slug + '_k3x9', ex;
          for (var salt = 0; modOf(nk); salt++) nk = slug + '_k3x' + salt;
          ex = exampleFolder(nk, nmName);
          disk[nk] = ex.folder;
          mods().push({key: nk, id: nk, name: nmName, author: '', version: '0.1.0', build: 1, size: 1530, files: 2, pointers: 0,
                       state: 'installed', cause: '', otherBuilds: 0, duplicateId: false, editorRows: 1, folder: nk, group: '',
                       description: '', origin: '', migratedFrom: '', servedBy: '', builtAt: Math.floor(Date.now() / 1000), bundle: '',
                       counts: {shadows: 0, sounds: 0, movies: 0, modules: 0, vt: 0}, requirements: {cvars: {}},
                       editorRowNames: ['Example crate'], carriedPaths: ex.folder.files.slice(1).map(function (f) { return f.path.slice(7); }),
                       publishedMaps: 0, newest: true});
          data.key = nk;
          data.file = ex.row;
          msg = 'Created ' + nmName + ' in mods\\' + nk + '. Its Example crate is in the Toybox.';
        }
        recount();
        data.ok = ok;
        data.message = msg;
        return [{kind: 'modsActionResult', action: a, ok: ok, message: msg, data: data}, {kind: 'mods', data: clone()}];
      }
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
      } else if (a === 'chooseRegistry') {
        var tab = key.indexOf('\t'), slot = tab < 0 ? key : key.slice(0, tab), pick = tab < 0 ? '' : key.slice(tab + 1);
        var cl = docu.map && Array.isArray(docu.map.registryClashes) ? docu.map.registryClashes.filter(function (c) { return c.slot === slot; })[0] : null;
        if (!cl) { ok = false; msg = 'The open map has no choice for ' + slot + '.'; }
        else {
          cl.chosen = pick;
          cl.serving = pick || (cl.providers[0] && cl.providers[0].id) || '';
          msg = pick ? slot + ' now comes from ' + pick + ' in this map; applies now; saved with the map\'s next save.'
                     : slot + ": the map's choice is cleared; the first mod in the map's list serves it.";
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
          msg = 'Uninstalled ' + nameOf(key) + (r ? ' and removed it from the open map' : '') + '. Its folder was moved to the Recycle Bin' +
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

  return {STATES: STATES, STATUS: STATUS, ORIGINS: ORIGINS, ACTIONS: ACTIONS, DUPLICATE_TEXT: DUPLICATE_TEXT,
          FILES_NOTE: FILES_NOTE, NEEDS_NOTE: NEEDS_NOTE, IN_MAP_TEXT: IN_MAP_TEXT, IN_MAP_GLYPH: IN_MAP_GLYPH, compareMods: compareMods,
          normalize: normalize, formatSize: formatSize, formatDate: formatDate, statusInfo: statusInfo, servingText: servingText,
          originText: originText, renamedText: renamedText, countLines: countLines, subText: subText,
          filterMods: filterMods, actionMessage: actionMessage, consequenceLines: consequenceLines, normPreview: normPreview,
          mapModStatus: mapModStatus, UNINSTALL_HEAD: UNINSTALL_HEAD, uninstallHead: uninstallHead, savedMapsText: savedMapsText,
          SCOPE: SCOPE, MAP_LEVEL_TEXT: MAP_LEVEL_TEXT, standIn: standIn, create: create,
          NOTHING_PLACED_TEXT: NOTHING_PLACED_TEXT, EDITOR_TWEAKS_TEXT: EDITOR_TWEAKS_TEXT,
          factLines: factLines, slotText: slotText,
          FILE_CLASSES: FILE_CLASSES, FILE_SOURCES: FILE_SOURCES, NEW_MOD_HINT: NEW_MOD_HINT, normFiles: normFiles,
          fileTree: fileTree, fileMeta: fileMeta};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = modsTab;
