/* The Mods tab: the player's mods and the mods downloaded maps brought, and the selected mod's
   details, with the few actions a player uses (install a folder, make a new mod, refresh, open the
   mods folder, use a mod in the open map or remove it from the map, uninstall). Browser
   global + CommonJS test export. Pure ASCII.

   The backend (backend/modui.c, the `mods` interface slot) builds one document on the game thread
   (payload v4, docs/webview-ui.md): one row per mod folder of mods\ (its key is the folder's path
   under mods\), the player's own at the root with their organizing folders ("group"), and each
   downloaded map's mods in that map's place ("place", "placeTitle"); and the open map (the map
   modal and the rows' in-this-map glyph). The list shows two kinds of groups: "Your mods", with
   the organizing folders inside it, and one group per downloaded map, titled with the map's name.
   Each row: the mod's name and size, one status glyph (needs attention, else in this
   map, else ready). The details pane: the description, what the mod adds (its Toybox rows by
   name), what it changes in the game (its edits, in words), what it borrows from the game (a
   count, expandable), the console settings it needs, its files, and, for a mod that cannot be
   used, the backend's one plain sentence saying what is wrong and what to do. A mod carrying the
   same files as another shows an informational hint. Nothing a player reads names a salt, a hash,
   a build number or the internal words; the salt reaches the page only for the Files view's
   paths, and an author never needs it. There is no folder watcher: Refresh rereads the folder, and
   the empty state and New mod say so. The host posts the document as {kind:'mods', data} after
   every action; showing the tab asks for a Refresh. Nothing polls. All text goes through
   textContent. */
var modsTab = (function () {
  var STATES = {
    ready: {label: 'Ready', tone: 'ok'},
    attention: {label: 'Needs attention', tone: 'warn'}
  };
  /* The row states a player sees: ready, or needs attention with its cause. */
  var STATUS = {
    ok:   {icon: 'check', label: 'Ready'},
    warn: {icon: 'triangle-alert', label: 'Needs attention'}
  };
  var YOUR_MODS = 'Your mods';
  var READY_TEXT = 'Ready to use.';
  /* A mod that cannot be used and whose cause the backend did not send. */
  var UNUSABLE_TEXT = 'This mod cannot be used right now; press Refresh.';
  function placeText(b) { return 'Came with the map ' + (b.placeTitle || 'you downloaded') + '; only that map uses it.'; }
  /* The informational hint: near-identical mods are two mods (identity is the salt); nothing merges. */
  function sameFilesText(s) {
    return 'Carries ' + plural(s.count, 'file') + ' that ' + s.name + ' also carries. They stay two separate mods.';
  }
  var IN_MAP_TEXT = 'In this map';
  var IN_MAP_TITLE = 'The open map uses this mod';
  /* The row's one glyph for a mod the open map uses (ready and in this map). */
  var IN_MAP_GLYPH = {tone: 'map', icon: 'map', label: IN_MAP_TEXT};
  /* There is no folder watcher: Refresh is how a changed mod is read again. */
  var REFRESH_TEXT = "After you change a mod's files, press Refresh to see the change.";
  /* An action whose files landed while the game did not take its part (the host's applyPending). */
  var APPLY_PENDING_TEXT = 'The game has not taken the change yet; press Refresh to bring it in.';
  var EMPTY_TEXT = 'No mods yet. Make your own with New mod, or add a mod folder or ZIP you have with Install.';
  /* The two *Preview actions are dry runs that answer with the consequence list the confirmation
     shows. mapAddMod attaches the selected mod to the open map without placing anything (a
     map-level row); it removes nothing, so it has no dry run. Uninstall moves the folder to the
     Windows Recycle Bin. */
  var ACTIONS = ['install', 'rescan', 'openFolder', 'openFile', 'mapAddMod', 'addToMine',
                 'mapRemoveModPreview', 'mapRemoveMod', 'uninstallPreview', 'uninstall', 'chooseRegistry',
                 'modFiles', 'modFileText', 'newMod'];
  /* The Files view's two reads change nothing: the host answers them without a fresh document. */
  var READS = {modFiles: true, modFileText: true};
  /* What each file of a mod is to the game, as one phrase (the backend's "class"). */
  var FILE_CLASSES = {
    descriptor: 'About the mod: its name, requirements and Toybox text',
    add: 'New file',
    borrow: "A game file under this mod's own name",
    edit: 'Your edit of a game file',
    replace: 'Replaces a game file',
    editor: 'An editor tweak, on this computer only',
    surface: 'A game file SnapMap cannot see on its own',
    '': 'Not read yet'
  };
  var NOT_SERVED_TITLE = 'Snapmap+ reads this file when you press Refresh in the Mods tab.';
  /* Where a game file's bytes come from: the archive the install bridge matched. */
  var FILE_SOURCES = {
    gameresources: "from the game's campaign and multiplayer files",
    snap_gameresources: "from the game's SnapMap files",
    loose: 'from a file in your install',
    '': 'from your install'
  };
  /* The quiet line above a new mod's example: its first steps. */
  var NEW_MOD_HINT = "Place Example crate from the Toybox in your map. To change the mod, edit this file in the mod's folder, then press Refresh in the Mods tab.";
  /* How an author names their own files and strings: by the mod's folder name, never its salt
     (Refresh files them under the mod). One sentence, where the author sees it. */
  function folderRuleText(folder) {
    var f = str(folder) || "<the mod's folder name>";
    return 'Put new files under assets\\mods\\' + f + '\\, and in your files write mods/' + f + '/ and plain string names.';
  }
  var NEW_MOD_NOTE = 'Snapmap+ makes a folder for it in mods\\ with an example crate you can place right away.';
  /* What removing a mod from a map severs: wires and timeline references to its placed entities.
     Other reference kinds are not consequences of the removal and are not shown. */
  var REF_KINDS = {wire: 'wire', timeline: 'timeline'};
  var MAP_STATES = {installed: 'ok', active: 'ok'};
  /* A map needs the files its rows list; a mod folder provides them (the asset rule). */
  var NOT_INSTALLED = 'not installed on this computer; playing the map downloads it';
  var MAP_CAUSES = {missing: NOT_INSTALLED, incomplete: 'installed, but it lacks files this map uses',
                    skipped: 'installed, but it cannot be used right now'};
  /* Under the Uninstall confirmation's scope line: what Ctrl+Z and the Recycle Bin can still do. */
  var UNINSTALL_HEAD = {
    undo: 'Undo history is cleared. The folder can be restored from the Recycle Bin by hand. Uninstalling:',
    plain: 'The folder can be restored from the Recycle Bin by hand. Uninstalling:'
  };
  function uninstallHead(pv) { return pv && pv.install && pv.install.undoCleared ? UNINSTALL_HEAD.undo : UNINSTALL_HEAD.plain; }
  /* The saved maps that use the mod (the dry run's savedMaps), and the undo history. */
  function savedMapsText(n) {
    return n + ' of your saved maps use it; they will not open until a mod folder provides its files again.';
  }
  /* The scope sentence under each confirmation's title, so the two dialogs read differently. */
  var SCOPE = {
    mapRemoveMod: 'Only this map changes. The mod stays installed on this computer and keeps its Toybox rows.',
    uninstallUsed: 'Removes the mod from this computer. The open map is cleaned of it first.',
    uninstallUnused: 'Removes the mod from this computer. The open map does not use it.'
  };
  function confirmScope(pv) { return pv.action === 'uninstall' ? (pv.map ? SCOPE.uninstallUsed : SCOPE.uninstallUnused) : SCOPE.mapRemoveMod; }
  var BORROWS_NOTE = "Game files are never stored or uploaded with the mod; Snapmap+ reads them from your install.";
  var NEEDS_NOTE = 'Every player needs Snapmap+ to play a map that uses this mod.';
  /* A folder of editor tweaks only (facts.editorOnly): a map never records it. */
  var EDITOR_ONLY_NOTE = "Changes only this computer's editor; maps made with it need nothing.";
  var USE_TITLE = 'Use the selected mod in the whole open map without placing anything; the map lists it when you save';
  /* A downloaded map's mod: the short view, its map, Add to my mods and Remove. */
  var ADD_MINE_TITLE = 'Copy this mod into your own mods, so your own maps can use it';
  var PLACE_CLEANUP_TEXT = 'Snapmap+ removes it at start when none of your saved maps uses it and you have not played ' +
                           'that map for 30 days; playing the map downloads it again.';
  var MAP_LEVEL_TEXT = 'Used by the whole map';
  var NOTHING_PLACED_TEXT = 'Used by the whole map, nothing placed';
  /* Editor tweaks always apply in this computer's editor, a downloaded map included; a map never
     records them and Play never sees them. */
  var EDITOR_TWEAKS_TEXT = 'they apply in your editor whatever map is open and are never saved with a map or used at Play';
  var NOTHING_CHANGED_TEXT = 'Changes nothing of the game; it only adds.';
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
  function normCvars(v) {
    var r = obj(v), c = r && obj(r.cvars);
    if (!c) return [];
    return Object.keys(c).sort().map(function (k) { return {name: k, value: str(c[k])}; });
  }
  /* requirements.commands: console lines run when the mod starts and stops. */
  function normCommands(v) {
    var r = obj(v), c = r && obj(r.commands);
    return {activate: c ? normNames(c.activate) : [], deactivate: c ? normNames(c.deactivate) : []};
  }
  /* The derived facts (payload facts): null when the backend did not send them (a mod that is not
     built). */
  function normFacts(v) {
    var f = obj(v);
    if (!f) return null;
    var reg = obj(f.registries) || {};
    return {adds: num(f.adds), editor: num(f.editor), replaces: num(f.replaces),
            editorOnly: bool(f.editorOnly), editorTweaks: normNames(f.editorTweaks),
            replacements: arr(f.replacements).map(function (r) { r = obj(r) || {}; return {path: str(r.path), edits: bool(r.edits)}; })
              .filter(function (r) { return r.path.length > 0; }),
            edits: arr(f.edits).map(function (e) {
              e = obj(e) || {};
              return {file: str(e.file), ops: num(e.ops), set: num(e.set), remove: num(e.remove), list: num(e.list),
                      keyed: num(e.keyed), bag: num(e.bag), drop: num(e.drop)};
            }).filter(function (e) { return e.file.length > 0; }),
            conductor: normNames(reg.conductor),
            stringEdits: normNames(reg.stringEdits),
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
  function normBorrows(v) {
    v = obj(v) || {};
    var paths = normNames(v.paths);
    return {count: Math.max(num(v.count), paths.length), paths: paths};
  }
  function normSame(v) {
    v = obj(v);
    return v && str(v.name) && num(v.count) > 0 ? {name: str(v.name), folder: str(v.folder), count: num(v.count)} : null;
  }
  function normMod(b) {
    b = b && typeof b === 'object' ? b : {};
    var state = has(STATES, b.state) ? b.state : 'attention';
    return {key: str(b.key), salt: str(b.salt), name: str(b.name), author: str(b.author),
            size: num(b.size), files: num(b.files), state: state, cause: str(b.cause),
            editorRows: num(b.editorRows), folder: str(b.folder),
            /* the organizing folder it sits in under mods\ ("" at the top, and for a map's mods) */
            group: str(b.group),
            /* a downloaded map's place ("maps\<segment>") and the map's name; "" for the player's own */
            place: str(b.place), placeTitle: str(b.placeTitle),
            description: str(b.description), cvars: normCvars(b.requirements), commands: normCommands(b.requirements),
            editorRowNames: normNames(b.editorRowNames),
            publishedMaps: b.publishedMaps === undefined || b.publishedMaps === null ? -1 : num(b.publishedMaps),
            facts: normFacts(b.facts), borrows: normBorrows(b.borrows), sameFilesAs: normSame(b.sameFilesAs),
            /* the builder's warnings in plain words; a warning is not attention */
            warnings: normNames(b.warnings)};
  }
  function title(b) { return b.name || b.folder || b.key; }
  function cmp(x, y) { return x < y ? -1 : x > y ? 1 : 0; }
  /* The list's one order inside a group: the display name without case, then the folder. */
  function compareMods(a, b) {
    return cmp(title(a).toLowerCase(), title(b).toLowerCase()) || cmp(a.key, b.key);
  }

  /* The host's payload as the tab uses it. error is set for an unusable payload. */
  function normalize(payload) {
    if (!payload || typeof payload !== 'object' || Array.isArray(payload))
      return {ok: false, error: 'The mods list is unavailable.', mods: [], root: '', gen: 0, v: 0, map: null};
    var mods = arr(payload.mods).map(normMod);
    mods.sort(compareMods);
    return {ok: true, error: payload.storeReadable === false ? 'The mods folder cannot be read.' : '',
            root: str(payload.root), gen: num(payload.gen), v: num(payload.v), mods: mods,
            map: normMap(payload.map)};
  }

  /* The open map (the document's map object). null: no map object at all; open false: no map is
     open (entities -1). Absent fields stay null and are left out of the modal. */
  function normRef(r) { r = obj(r) || {}; return {kind: str(r.kind), name: str(r.name)}; }
  function normRefs(v) { return arr(v).map(normRef).filter(function (r) { return has(REF_KINDS, r.kind); }); }
  function normMapMod(m) {
    m = obj(m) || {};
    var names = normNames(m.placedEntities);
    return {key: str(m.key), salt: str(m.salt), name: str(m.name), state: str(m.state) || 'missing',
            placedEntities: names, placedCount: Math.max(num(m.placedCount), names.length),
            references: normRefs(m.references), size: num(m.size), attached: bool(m.attached),
            nothingPlaced: bool(m.nothingPlaced)};
  }
  function normClash(c) {
    c = obj(c) || {};
    return {slot: str(c.slot), chosen: str(c.chosen), serving: str(c.serving),
            providers: arr(c.providers).map(function (p) { p = obj(p) || {}; return {id: str(p.id), name: str(p.name) || 'a mod'}; })
              .filter(function (p) { return p.id.length > 0; })};
  }
  function normMap(m) {
    m = obj(m);
    if (!m) return null;
    var open = m.entities !== -1 || !!m.name;
    var pub = obj(m.published), up = obj(m.uploaded);
    return {open: open, name: str(m.name), saved: num(m.saved), entities: num(m.entities),
            published: pub ? {state: str(pub.state), at: num(pub.at)} : null,
            uploaded: up ? {claimed: bool(up.claimed), at: num(up.at)} : null,
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
                            /* another folder of the mod that stays installed: maps open with it */
                            keptBuild: str(ins.keptBuild)} : null,
            /* uninstallPreview only: the Toybox rows uninstalling takes out of the editor */
            editorRows: normNames(v.editorRows)};
  }
  function mapModStatus(r) {
    var tone = MAP_STATES[r.state] || 'warn';
    var cause = tone === 'ok' ? STATUS.ok.label : (MAP_CAUSES[r.state] || MAP_CAUSES.skipped);
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
  /* The row's second line: the size. */
  function subText(b) {
    return formatSize(b.size);
  }

  /* One of the two states, its one-line cause (the icon's hover title) and the details sentence. */
  function statusInfo(b) {
    if (b.state !== 'ready') {
      var cause = b.cause || UNUSABLE_TEXT;
      return {tone: 'warn', icon: STATUS.warn.icon, label: STATUS.warn.label, cause: cause, sentence: cause};
    }
    var ok = b.place ? placeText(b) : READY_TEXT;
    return {tone: 'ok', icon: STATUS.ok.icon, label: STATUS.ok.label, cause: ok, sentence: ok};
  }
  function needsAttention(b) { return statusInfo(b).tone === 'warn'; }

  function filterMods(mods, query) {
    var q = str(query).trim().toLowerCase();
    if (!q) return mods.slice();
    return mods.filter(function (b) { return (b.name + '\n' + b.folder + '\n' + b.group + '\n' + b.placeTitle).toLowerCase().indexOf(q) !== -1; });
  }

  /* A name as the page shows it: without the mod's own namespace (mods/<salt>/), which only the
     Files view's paths show. */
  function bareName(text, salt) {
    var t = str(text);
    if (!salt) return t;
    var at = t.toLowerCase().indexOf('mods/' + salt.toLowerCase() + '/');
    return at < 0 ? t : t.slice(0, at) + t.slice(at + salt.length + 6);
  }

  /* What the edits of one game file do, each part with its kind: 'set' (fields set, entries added
     or replaced) or 'removed' (fields or entries removed). */
  function editParts(e) {
    var parts = [];
    if (e.set) parts.push({text: plural(e.set, 'field set', 'fields set'), kind: 'set'});
    if (e.remove) parts.push({text: plural(e.remove, 'field removed', 'fields removed'), kind: 'removed'});
    if (e.list) parts.push({text: plural(e.list, 'list entry', 'list entries') + ' added', kind: 'set'});
    if (e.keyed) parts.push({text: plural(e.keyed, 'named entry', 'named entries') + ' added or replaced', kind: 'set'});
    if (e.bag) parts.push({text: plural(e.bag, 'entry', 'entries') + ' added', kind: 'set'});
    if (e.drop) parts.push({text: plural(e.drop, 'entry', 'entries') + ' removed', kind: 'removed'});
    return parts;
  }
  function editFile(e) { return e.file.replace(/^generated\/decls\//, '').replace(/\.decl$/, ''); }
  var NO_EDIT_TEXT = 'no change from the game\'s text';
  /* One edited game file: its name and what the edits do. */
  function editLine(e) {
    var parts = editParts(e);
    return editFile(e) + ': ' + (parts.length ? parts.map(function (p) { return p.text; }).join(', ') : NO_EDIT_TEXT);
  }

  /* Colour in the details pane (docs/webview-ui.md): the Decl Text editor's token hues, each naming
     one kind, each with a hover title. A path splits into its folder (dimmed), the mod's own
     namespace mods/<key>/ (the string hue) and the file name in the text colour; a path without that
     namespace is the game's, its file name in the keyword hue. */
  var PATH_TITLES = {own: 'the mod\'s own file', edit: 'a game file this mod edits', replace: 'a game file this mod replaces',
                     borrow: 'a game file this mod reads from your install'};
  function ownSpan(path, keys) {
    var p = '/' + str(path);
    for (var i = 0; i < keys.length; i++) {
      if (!keys[i]) continue;
      var at = p.indexOf('/mods/' + keys[i] + '/');
      if (at >= 0) return {at: at, len: keys[i].length + 6};
    }
    return null;
  }
  /* [{text, cls}] for one path: cls is 'mk-dir', 'mk-own', 'mk-game' or '' (the text colour). */
  function pathParts(path, keys) {
    path = str(path);
    var out = [], own = ownSpan(path, arr(keys));
    function push(text, cls) { if (text) out.push({text: text, cls: cls}); }
    var rest = own ? path.slice(own.at + own.len) : path, cut = rest.lastIndexOf('/') + 1;
    if (own) { push(path.slice(0, own.at), 'mk-dir'); push(path.substr(own.at, own.len), 'mk-own'); }
    push(rest.slice(0, cut), 'mk-dir');
    push(rest.slice(cut), own ? '' : 'mk-game');
    return out;
  }
  function modKeys(b) { return [b.salt]; }
  function pathKind(path, b) {
    if (ownSpan(path, modKeys(b))) return 'own';
    var edited = b.facts && b.facts.edits.some(function (e) { return e.file === path; });
    return edited ? 'edit' : 'replace';
  }
  /* A mod folder's path under mods\ for Open in Explorer: the row's key. */
  function folderArg(b) { return b.key; }
  /* A console value as the editor shows it: a number in the number hue, anything else a string. */
  function valueClass(v) { return /^-?\d+(\.\d+)?$/.test(str(v).trim()) ? 'mk-num' : 'mk-str'; }

  /* What the mod changes in the game, as {text, list?, kind?, items?} lines in the details pane's
     order: the game files it edits (in words), the texts it changes, the game files a received
     folder replaces, the encounters it lists, the game sound banks it loads, and its editor tweaks. */
  function changeLines(b) {
    var f = b.facts, out = [];
    if (!f) return out;
    if (f.edits.length) {
      out.push({text: 'Edits ' + plural(f.edits.length, 'game file') + '; a map that uses this mod gets the game\'s text with these edits.',
                list: f.edits.map(editLine), kind: 'edits', items: f.edits});
    }
    /* A strings key the game or Snapmap+ defines changes that text where the mod's edits apply. */
    if (f.stringEdits.length) {
      out.push({text: 'Changes the text of ' + plural(f.stringEdits.length, 'game string') + (f.editorOnly || f.editor
                  ? ' in your editor.' : ' in a map that uses it.'),
                list: f.stringEdits.map(function (k) { return '#str_' + k; }), kind: 'strings'});
    }
    /* An edited copy is an edit, never a replacement. */
    var other = f.replacements.filter(function (r) { return !r.edits; });
    if (other.length || f.replaces > 0) {
      out.push({text: 'Replaces ' + plural(Math.max(other.length, f.replaces), 'game file') + ' for the maps that use it.',
                list: other.map(function (r) { return r.path; }), kind: 'replaces'});
    }
    if (f.conductor.length) {
      out.push({text: 'Adds ' + plural(f.conductor.length, 'encounter') + ' to the encounter list of a map that uses it.',
                list: f.conductor.map(function (c) { return bareName(c, b.salt); }), kind: 'encounters',
                items: f.conductor.map(function (c) { return bareName(c, b.salt); })});
    }
    if (f.stockBanks.length) {
      out.push({text: "Loads the game's " + f.stockBanks.map(function (s) { return s.bank; }).join(', ') +
                ' sounds, which SnapMap leaves out, for a map that uses it.',
                list: f.stockBanks.map(function (s) {
                  return s.bank + ': ' + (s.state === 'loaded' && s.ms ? 'loaded (' + s.ms + ' ms)' :
                                          s.state === 'failed' ? 'did not load (error ' + s.result + ')' : s.state);
                }), kind: 'banks', items: f.stockBanks});
    }
    if (f.editor) out.push({text: 'Tweaks ' + plural(f.editor, 'editor file') + ' on this computer only: ' + EDITOR_TWEAKS_TEXT + '.',
                            list: f.editorTweaks});
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
              titleNode: null, stateNode: null, selNode: null};
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
    function mark(box, text, cls, tip) {
      var s = el('span', cls, text);
      if (tip) s.title = tip;
      box.appendChild(s);
      return s;
    }
    function markPath(box, path, keys, noGame) {
      pathParts(path, keys).forEach(function (p) { mark(box, p.text, noGame && p.cls === 'mk-game' ? '' : p.cls); });
    }
    /* One path line in the details pane; the line's hover names its kind. */
    function pathLine(path, b, kind) {
      var d = el('div', 'mods-path');
      d.title = PATH_TITLES[kind || pathKind(path, b)];
      markPath(d, path, modKeys(b));
      return d;
    }
    /* One entry of a fact's list, coloured by the fact's kind. */
    function factItem(l, i, b) {
      var item = l.items ? l.items[i] : null, d;
      if (l.kind === 'replaces') return pathLine(l.list[i], b, 'replace');
      if (l.kind === 'edits') {
        d = el('div', 'mods-path');
        var name = d.appendChild(el('span', ''));
        name.title = PATH_TITLES.edit;
        markPath(name, editFile(item), []);
        mark(d, ': ', '');
        var parts = editParts(item);
        if (!parts.length) mark(d, NO_EDIT_TEXT, '');
        parts.forEach(function (p, j) {
          if (j) mark(d, ', ', '');
          if (p.kind === 'set') mark(d, p.text, 'mk-set', 'what this mod sets or adds in the game\'s text');
          else mark(d, p.text, 'mk-removed', 'what this mod removes from the game\'s text');
        });
        return d;
      }
      if (l.kind === 'encounters') {
        d = el('div', 'mods-path');
        d.title = 'an encounter this mod adds';
        mark(d, 'encounter ', '');
        mark(d, item, 'mk-str');
        return d;
      }
      if (l.kind === 'banks') {
        d = el('div', 'mods-path');
        mark(d, item.bank, 'mk-game', 'a game sound bank, read from your install');
        mark(d, l.list[i].slice(item.bank.length), '');
        return d;
      }
      return el('div', '', l.list[i]);
    }

    function setStatus(text, tone) {
      var s = byId('modsStatus');
      if (!s) return;
      s.textContent = str(text);
      s.className = 'mods-status' + (tone ? ' ' + tone : '');
    }
    /* Install, New mod and Refresh build off the game's thread for as long as it takes; the host
       says when the game takes the change (modsProgress, phase "game"). */
    var GAME_PHASE_TEXT = 'Bringing the change into the game...';
    var BUSY_TEXT = {install: 'Installing and building...', rescan: 'Reading your mods and building what changed...',
                     mapRemoveModPreview: 'Reading what removing it changes...', uninstallPreview: 'Reading what uninstalling it changes...',
                     mapRemoveMod: 'Removing the mod from the map...', uninstall: 'Uninstalling...',
                     mapAddMod: 'Adding the mod to the map...', chooseRegistry: 'Saving the choice...',
                     newMod: 'Making the new mod...', addToMine: 'Copying it into your mods...'};

    function selectedMod() {
      for (var i = 0; i < model.mods.length; i++) if (model.mods[i].key === selectedKey) return model.mods[i];
      return null;
    }
    function attentionCount() { return model.mods.filter(needsAttention).length; }

    function renderButtons() {
      var b = selectedMod(), sel = !!b;
      ['modsInstallBtn', 'modsFolderBtn', 'modsNewBtn', 'modsRefreshBtn', 'modsUninstallBtn'].forEach(function (id) {
        var n = byId(id);
        if (n) n.disabled = !!busy || (!sel && id === 'modsUninstallBtn');
      });
      /* Files, the details head's left-most button (owner 2026-11-09), needs only a selection. */
      var fb = byId('modsFilesBtn');
      if (fb) { fb.disabled = !sel; fb.style.display = sel && b.place ? 'none' : ''; }
      /* A downloaded map's mod is removed from this computer, the same uninstall under its short name. */
      var un = byId('modsUninstallBtn');
      if (un) {
        un.textContent = sel && b.place ? 'Remove' : 'Uninstall';
        un.title = sel && b.place ? 'Remove this mod of a downloaded map from this computer'
                                  : 'Uninstall the selected mod; you see everything it changes before you decide';
      }
      var mb = byId('modsMapBtn');
      if (mb) {
        var mm = model.map && model.map.open && model.map.mods ? model.map.mods.length : -1;
        mb.disabled = !loaded;
        mb.title = mm >= 0 ? 'This map: its details and the ' + plural(mm, 'mod') + ' it uses' : 'This map: its details and the mods it uses';
        mb.setAttribute('aria-label', mb.title);
      }
      renderUse();
      modalButtons.forEach(function (n) { n.disabled = !!busy; });
    }
    /* The details head's map action: Use in this map, or Remove from map once the open map uses the
       selected mod. Shown only while a map is open in the editor. */
    function mapUses(b) {
      var m = model.map;
      if (!b || !m || !m.mods) return null;
      for (var i = 0; i < m.mods.length; i++) if (m.mods[i].key && m.mods[i].key === b.key) return m.mods[i];
      return null;
    }
    function editorOnly(b) { return !!(b && b.facts && b.facts.editorOnly); }
    function renderUse() {
      var u = byId('modsUseBtn');
      if (!u) return;
      var b = selectedMod(), m = model.map, open = loaded && !!m && m.open && !!m.mods;
      /* A downloaded map's mod: Add to my mods copies it to the root, where your own maps use it. */
      if (b && b.place) {
        u.style.display = '';
        u.textContent = 'Add to my mods';
        u.className = 'btn';
        u.title = ADD_MINE_TITLE;
        u.disabled = !!busy || needsAttention(b);
        return;
      }
      /* A folder of editor tweaks only has no map action: a map never records it. */
      u.style.display = open && !editorOnly(b) ? '' : 'none';
      var used = mapUses(b);
      u.textContent = used ? 'Remove from map' : 'Use in this map';
      u.className = 'btn' + (used ? ' danger' : '');
      u.title = used ? 'Remove the selected mod and everything that uses it from this map' : USE_TITLE;
      u.disabled = !!busy || !b || !open || (!used && needsAttention(b));
    }
    function setBusy(action) {
      busy = action || '';
      renderButtons();
      if (busy) setStatus(BUSY_TEXT[busy] || 'Working...', 'busy');
    }

    /* ---- the list: two kinds of groups, every row shown mounted. "Your mods" holds the player's
       own mods, an organizing folder of mods\ under its own caret row (the Prefabs tab's caret and
       count), folders first; then one group per downloaded map, titled with the map's name, holding
       the mods that map brought. A closed group hides its rows for the session. view is the rows
       shown, in the order the arrow keys walk. */
    function computeView() {
      var shown = filterMods(model.mods, filter);
      if (attnOnly) shown = shown.filter(needsAttention);
      view = shown;
      return shown;
    }
    function groupClosed(g) { return closedGroups[g] === true && !str(filter).trim() && !attnOnly; }
    /* One collapsible group: an area ('area' kind: Your mods, or a map) or an organizing folder. */
    function groupRow(id, label, members, fill, area) {
      var closed = groupClosed(id);
      var f = el('div', 'pf-folder ' + (area ? 'mods-area' : 'mods-folder') + (closed ? '' : ' pf-open'));
      f.setAttribute('data-group', id);
      var head = el('div', 'pf-folder-head' + (area ? ' mods-area-head' : ''));
      head.setAttribute('role', 'button');
      head.setAttribute('aria-expanded', closed ? 'false' : 'true');
      head.title = area ? label : 'mods\\' + id;
      var caret = el('span', 'pf-caret');
      caret.appendChild(icon('chevron-down'));
      head.appendChild(caret);
      head.appendChild(el('span', 'pf-fname', label));
      head.appendChild(el('span', 'pf-fcount', String(members.length)));
      var attn = members.filter(needsAttention).length;
      if (closed && attn) {
        var w = statusIcon({tone: 'warn', icon: STATUS.warn.icon, label: plural(attn, 'mod needs', 'mods need') + ' attention'});
        w.title = w.getAttribute('aria-label');
        head.appendChild(w);
      }
      head.addEventListener('click', function () { closedGroups[id] = !groupClosed(id); renderList(); });
      f.appendChild(head);
      var body = el('div', 'pf-folder-body');
      if (!closed) fill(body);
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
    function areaId(b) { return b.place ? 'map:' + b.place : 'area:own'; }
    function renderList() {
      var box = byId('modsList');
      if (!box) return;
      clear(box);
      rowNodes = {};
      var shown = computeView();
      if (!loaded) { box.appendChild(el('div', 'list-empty', waiting ? 'Reading your mods...' : 'Open this tab to list your mods.')); return; }
      if (lastError) { box.appendChild(el('div', 'list-empty mods-error', lastError)); return; }
      if (!model.mods.length) {
        var card = el('div', 'list-empty mods-empty');
        card.appendChild(el('div', '', EMPTY_TEXT));
        card.appendChild(el('div', '', REFRESH_TEXT));
        card.appendChild(newModButton());
        box.appendChild(card);
        return;
      }
      if (!shown.length) {
        box.appendChild(el('div', 'list-empty', attnOnly ? 'No mod that needs attention matches the filter.' : 'No mod matches the filter.'));
        return;
      }
      var own = shown.filter(function (b) { return !b.place; });
      var places = {}, placeIds = [];
      shown.forEach(function (b) {
        if (!b.place) return;
        if (!has(places, b.place)) { places[b.place] = []; placeIds.push(b.place); }
        places[b.place].push(b);
      });
      placeIds.sort(function (x, y) {
        var a = places[x][0].placeTitle || x, c = places[y][0].placeTitle || y;
        return cmp(a.toLowerCase(), c.toLowerCase()) || cmp(x, y);
      });
      view = [];
      if (own.length) {
        box.appendChild(groupRow('area:own', YOUR_MODS, own, function (body) {
          var groups = {}, names = [], root = [];
          own.forEach(function (b) {
            if (!b.group) { root.push(b); return; }
            if (!has(groups, b.group)) { groups[b.group] = []; names.push(b.group); }
            groups[b.group].push(b);
          });
          names.sort(function (x, y) { return cmp(x.toLowerCase(), y.toLowerCase()) || cmp(x, y); });
          names.forEach(function (g) {
            body.appendChild(groupRow(g, g.replace(/\\/g, '/'), groups[g], function (inner) {
              groups[g].forEach(function (b) { inner.appendChild(modRow(b)); view.push(b); });
            }, false));
          });
          root.forEach(function (b) { body.appendChild(modRow(b)); view.push(b); });
        }, true));
      }
      placeIds.forEach(function (p) {
        var members = places[p];
        box.appendChild(groupRow('map:' + p, members[0].placeTitle || p.replace(/^maps\\/, ''), members, function (body) {
          members.forEach(function (b) { body.appendChild(modRow(b)); view.push(b); });
        }, true));
      });
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

    /* Open the groups a mod sits in; 1 when one was closed. */
    function openGroupsOf(b) {
      var opened = 0;
      if (!b) return 0;
      if (groupClosed(areaId(b))) { closedGroups[areaId(b)] = false; opened = 1; }
      if (b.group && groupClosed(b.group)) { closedGroups[b.group] = false; opened = 1; }
      return opened;
    }
    function select(key) {
      selectedKey = key;
      /* A mod in a closed group opens its group. */
      if (openGroupsOf(selectedMod())) renderList();
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
      card.appendChild(el('p', '', folderRuleText('')));
      card.appendChild(el('p', '', 'Maps record which mods they use. Players who open one are offered the download, which is listed under the map\'s name.'));
      card.appendChild(el('p', '', REFRESH_TEXT));
      card.appendChild(el('p', '', empty ? 'To add a mod, make one with New mod, or add a mod folder or ZIP with Install.'
                                         : 'Select a mod to see what it adds.'));
      box.appendChild(card);
    }
    /* A list line that opens to show its names. */
    function listLine(box, text, items, item) {
      if (!items || !items.length) { box.appendChild(el('div', 'mods-line', text)); return; }
      var d = el('details', 'pc-limits-details mods-paths');
      d.appendChild(el('summary', '', text));
      var dl = el('div', 'mods-paths-list');
      items.forEach(function (n, i) { dl.appendChild(item ? item(i) : el('div', '', n)); });
      d.appendChild(dl);
      box.appendChild(d);
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
      /* A downloaded map's mod: the short view. A player who only plays maps needs its name, its
         map and Remove; Add to my mods (the head) makes it one of their own. */
      if (b.place) {
        var shortAbout = section(box, 'about', 'About');
        field(shortAbout, 'Came with', b.placeTitle || 'a downloaded map');
        shortAbout.appendChild(el('div', 'mods-note', PLACE_CLEANUP_TEXT));
        return;
      }
      if (b.sameFilesAs) box.appendChild(el('div', 'mods-note mods-same', sameFilesText(b.sameFilesAs)));

      var about = section(box, 'about', 'About');
      if (b.description) field(about, 'Description', b.description).className += ' mods-field-long';
      if (b.author) field(about, 'Author', b.author);
      var link = el('button', 'mods-link', b.key);
      link.type = 'button';
      link.title = 'Open this folder in Explorer';
      link.addEventListener('click', function () { act('openFolder', folderArg(b)); });
      field(about, 'Folder', link);

      var adds = section(box, 'adds', 'What it adds');
      if (b.editorRowNames.length) {
        var list = el('div', 'mods-rows-list');
        b.editorRowNames.forEach(function (n) { list.appendChild(el('div', 'mods-rows-item mk-str', n)).title = 'a Toybox row this mod adds'; });
        field(adds, plural(b.editorRowNames.length, 'Toybox row'), list);
      } else if (b.editorRows > 0) {
        adds.appendChild(el('div', 'mods-line', plural(b.editorRows, 'Toybox row')));
      } else adds.appendChild(el('div', 'mods-line muted', 'Adds no Toybox rows.'));

      if (b.facts) {
        var changes = section(box, 'changes', 'What it changes in the game');
        var lines = changeLines(b);
        if (!lines.length) changes.appendChild(el('div', 'mods-line muted', NOTHING_CHANGED_TEXT));
        lines.forEach(function (l) { listLine(changes, l.text, l.list, function (i) { return factItem(l, i, b); }); });

        var borrows = section(box, 'borrows', 'What it borrows from the game');
        if (!b.borrows.count) borrows.appendChild(el('div', 'mods-line muted', 'Borrows nothing; every file is its own.'));
        else {
          listLine(borrows, plural(b.borrows.count, 'game file') + ', read from your install', b.borrows.paths, function (i) {
            return pathLine(b.borrows.paths[i], b, 'borrow');
          });
          if (b.borrows.count > b.borrows.paths.length)
            borrows.appendChild(el('div', 'mods-line muted', 'and ' + (b.borrows.count - b.borrows.paths.length) + ' more'));
          borrows.appendChild(el('div', 'mods-note', BORROWS_NOTE));
        }
      }

      var req = section(box, 'requirements', 'Console settings');
      if (b.cvars.length) {
        var cl = el('div', 'mods-rows-list mono');
        b.cvars.forEach(function (c) {
          var r = cl.appendChild(el('div', ''));
          mark(r, c.name, 'mk-key', 'a console setting this mod needs');
          mark(r, ' ' + c.value, valueClass(c.value), 'the value it is set to while a map uses this mod');
        });
        field(req, 'Needs', cl);
      } else req.appendChild(el('div', 'mods-line muted', 'Needs no console settings.'));
      [['activate', 'Commands when used'], ['deactivate', 'Commands when no longer used']].forEach(function (k) {
        var cmds = b.commands[k[0]];
        if (!cmds.length) return;
        var ll = el('div', 'mods-rows-list mono');
        cmds.forEach(function (line) { ll.appendChild(el('div', '', line)); });
        field(req, k[1], ll);
      });
      req.appendChild(el('div', 'mods-note', editorOnly(b) ? EDITOR_ONLY_NOTE : NEEDS_NOTE));

      var files = section(box, 'files', 'Files');
      field(files, 'In its folder', plural(b.files, 'file') + ', ' + formatSize(b.size));
      var fv = el('button', 'mods-link', 'Show the files');
      fv.type = 'button';
      fv.title = "Browse this mod's files";
      fv.addEventListener('click', function () { openFiles(b.key, fv); });
      files.appendChild(fv);

      if (b.warnings.length) {
        var wl = section(box, 'warnings', 'Warnings');
        b.warnings.forEach(function (w) { wl.appendChild(el('div', 'mods-line mods-warning', w)); });
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
    function modName(key, name) {
      for (var i = 0; i < model.mods.length; i++) if (key && model.mods[i].key === key) return title(model.mods[i]);
      return name || 'A mod that is not installed';
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
      if (m.published) {
        var pubd = m.published.state === 'published';
        field(det, 'Published', pubd ? (m.published.at ? 'Yes, ' + formatDate(m.published.at) : 'Yes') : 'Not published');
      }
      if (m.uploaded) field(det, 'Its mods', m.uploaded.claimed
        ? 'Uploaded with the published map' + (m.uploaded.at ? ' (' + formatDate(m.uploaded.at) + ')' : '')
        : 'Not uploaded');
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
          /* No choice: the first mod the map lists decides, and this option clears a choice. */
          var first = el('option', '', 'The first mod the map lists (' + c.providers[0].name + ')');
          first.value = '';
          if (!c.chosen) first.selected = true;
          sel.appendChild(first);
          c.providers.forEach(function (p) {
            var o = el('option', '', p.name);
            o.value = p.id;
            if (c.chosen && p.id === c.chosen) o.selected = true;
            sel.appendChild(o);
          });
          sel.value = c.chosen;
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
      row.setAttribute('data-key', r.key || r.salt);
      row.title = info.cause;
      row.appendChild(statusIcon(info));
      var text = el('div', 'mods-row-text');
      text.appendChild(el('div', 'mods-row-name', modName(r.key, r.name)));
      text.appendChild(el('div', 'mods-row-sub', formatSize(r.size)));
      var uses = r.nothingPlaced ? NOTHING_PLACED_TEXT : r.attached
        ? MAP_LEVEL_TEXT + (r.placedCount ? ', and ' + plural(r.placedCount, 'placed entity', 'placed entities') + (r.placedCount === 1 ? ' uses it' : ' use it') : '')
        : plural(r.placedCount, 'placed entity', 'placed entities') + (r.placedCount === 1 ? ' uses it' : ' use it');
      text.appendChild(el('div', 'mods-map-uses' + (r.attached ? ' mods-map-level' : ''), uses + (info.tone === 'warn' ? '; ' + info.cause : '')));
      if (r.attached) row.setAttribute('data-attached', 'true');
      if (r.references.length) text.appendChild(el('div', 'mods-map-uses', 'Also used by ' + r.references.map(refText).join(', ')));
      row.appendChild(text);
      var name = modName(r.key, r.name);
      row.appendChild(modalButton('Remove from map', 'danger', function () {
        modal.from = 'map';
        act('mapRemoveModPreview', r.key || r.salt);
      }, 'Remove ' + name + ' and everything that uses it from this map'));
      return row;
    }
    function renderConfirm(host, pv) {
      var name = pv.name || modName(pv.key, '');
      var uninstall = pv.action === 'uninstall';
      var placed = false;
      model.mods.forEach(function (b) { if (b.key === pv.key && b.place) placed = true; });
      var verb = placed ? 'Remove' : 'Uninstall';
      modalHead(host, uninstall ? verb + ' ' + name + '?' : 'Remove ' + name + ' from ' + (pv.map && pv.map.name ? pv.map.name : 'this map') + '?');
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
      acts.appendChild(modalButton(uninstall ? verb : 'Remove from map', 'danger-solid', function () {
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
      read('modFiles', key);
    }
    function closeFiles() {
      if (!fm.open) return;
      fm.open = false;
      filesOverlay(false);
      clear(byId('modsFilesHost'));
      fm.list = fm.pane = fm.closeBtn = fm.titleNode = fm.stateNode = fm.selNode = null;
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
      if (!inside(byId('modsFilesModal'), doc.activeElement)) focusIn(fm.closeBtn);
    }
    /* A render rebuilds the node that had focus, which then falls to the page's body, outside the
       modal, where Escape and Tab no longer reach the modal's own listener. So focus is put back:
       on the selected file when it was in the tree, else on the close button. */
    function inside(node, n) {
      for (; node && n; n = n.parentNode) if (n === node) return true;
      return false;
    }
    function focusIn(n) { if (n && n.focus) n.focus(); }
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
      if (on) fm.selNode = r;
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
      var hadFocus = inside(box, doc.activeElement);
      clear(box);
      fm.visible = [];
      fillFileTree(box);
      if (hadFocus || !inside(byId('modsFilesModal'), doc.activeElement)) focusIn(fm.selNode || fm.closeBtn);
    }
    function fillFileTree(box) {
      fm.selNode = null;
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
      /* The view is read-only: Open in editor hands a file of the folder to the system's editor. */
      if (f.inFolder) {
        var ed = el('button', 'btn mf-open', 'Open in editor');
        ed.type = 'button';
        ed.title = 'Open this file in your editor; press Refresh in the Mods tab after you save it';
        ed.addEventListener('click', function () { act('openFile', fm.key + '\t' + f.path); });
        head.appendChild(ed);
      }
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
      if (e.shiftKey && (at === first || !inside(host, at))) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && (at === last || !inside(host, at))) { e.preventDefault(); first.focus(); }
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
        openGroupsOf(selectedMod());
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
      if (action !== 'openFolder' && action !== 'openFile') setBusy(action);
      env.post(m);
      return true;
    }
    /* Refresh, the one rescan: every changed folder is built again and the change brought into the
       game; the host follows it with a fresh document. Quiet: its result shows only when it failed
       or when a change still needs something (a restart, the next map open). A running action
       already ends with a fresh document. */
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
      /* The file work landed but the game did not take its part yet: Refresh brings it in. */
      if (d.applyPending) message = (message ? message + ' ' : '') + APPLY_PENDING_TEXT;
      if (wasQuiet) {
        /* A failed rescan is followed by a document unless the host cannot reach the backend at all. */
        waiting = false;
        if (!ok && !loaded) { loaded = true; lastError = message || 'The mods list is unavailable.'; renderAll(); }
        setStatus(message, ok ? (message ? 'ok' : '') : 'err');
        if (message) env.toast(message, ok ? 'ok' : 'err');
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
      if ((action === 'install' || action === 'addToMine') && ok && data.key) reveal = selectedKey = str(data.key);
      /* A new mod: selected, and the Files modal opens on its example's Toybox row. */
      if (action === 'newMod' && ok && data.key) {
        reveal = selectedKey = str(data.key);
        var made = selectedKey.slice(selectedKey.lastIndexOf('\\') + 1);
        openFiles(selectedKey, byId('modsFilesBtn'), str(data.file), NEW_MOD_HINT + ' ' + folderRuleText(made));
      }
      if (action === 'uninstall' && ok) selectedKey = '';
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
      /* Install takes mod folders or mod ZIPs: the button asks which, then the host's picker opens. */
      on('modsInstallBtn', 'click', function () {
        if (busy) return;
        var n = byId('modsInstallBtn'), r = n && n.getBoundingClientRect ? n.getBoundingClientRect() : null;
        var items = [{label: 'Install mod folders...', run: function () { act('install'); }},
                     {label: 'Install mod ZIPs...', run: function () { act('install', 'zip'); }}];
        if (env.menu) env.menu(r ? r.left : 0, r ? r.bottom : 0, items);
        else act('install');
      });
      on('modsFolderBtn', 'click', function () { act('openFolder'); });
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
        if (b.place) act('addToMine', b.key);
        else if (mapUses(b)) {
          modal.from = 'details';
          modal.opener = byId('modsUseBtn');
          act('mapRemoveModPreview', b.key);
        } else act('mapAddMod', b.key);
      });
      on('modsMapModal', 'keydown', onModalKey);
      on('modsMapModal', 'mousedown', function (e) { if (e.target === byId('modsMapModal')) closeMap(); });
      on('modsRefreshBtn', 'click', function () { refresh(); });
      on('modsFilesBtn', 'click', function () { var b = selectedMod(); if (b) openFiles(b.key, byId('modsFilesBtn')); });
      on('modsFilesModal', 'keydown', onFilesKey);
      /* While the Files modal is open, Escape and Tab from anywhere in the page are its own, as the
         expanded Decl Text modal's Escape is (a key from inside reaches the modal's listener). */
      if (doc.addEventListener) doc.addEventListener('keydown', function (e) {
        if (fm.open && !inside(byId('modsFilesModal'), e.target)) onFilesKey(e);
      });
      on('modsFilesModal', 'mousedown', function (e) { if (e.target === byId('modsFilesModal')) closeFiles(); });
      on('modsNewModal', 'keydown', onNewKey);
      on('modsNewModal', 'mousedown', function (e) { if (e.target === byId('modsNewModal')) closeNewMod(); });
      on('modsNewCancel', 'click', closeNewMod);
      on('modsNewCreate', 'click', createNewMod);
      /* New mod: the header's button, the empty list's button and the list's right-click entry. */
      on('modsNewBtn', 'click', function () { openNewMod(byId('modsNewBtn')); });
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
      /* The tab became visible: reread the folder (explicit demand, never a timer; there is no watcher). */
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
        if (d.kind === 'modsProgress') { if (busy && d.phase === 'game') setStatus(GAME_PHASE_TEXT, 'busy'); return true; }
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

  return {STATES: STATES, STATUS: STATUS, ACTIONS: ACTIONS, YOUR_MODS: YOUR_MODS, READY_TEXT: READY_TEXT, REFRESH_TEXT: REFRESH_TEXT,
          APPLY_PENDING_TEXT: APPLY_PENDING_TEXT,
          EMPTY_TEXT: EMPTY_TEXT, BORROWS_NOTE: BORROWS_NOTE, NEEDS_NOTE: NEEDS_NOTE, EDITOR_ONLY_NOTE: EDITOR_ONLY_NOTE,
          IN_MAP_TEXT: IN_MAP_TEXT, IN_MAP_GLYPH: IN_MAP_GLYPH, compareMods: compareMods, placeText: placeText,
          sameFilesText: sameFilesText, bareName: bareName, PLACE_CLEANUP_TEXT: PLACE_CLEANUP_TEXT, ADD_MINE_TITLE: ADD_MINE_TITLE, folderRuleText: folderRuleText,
          normalize: normalize, formatSize: formatSize, formatDate: formatDate, statusInfo: statusInfo, subText: subText,
          filterMods: filterMods, actionMessage: actionMessage, consequenceLines: consequenceLines, normPreview: normPreview,
          mapModStatus: mapModStatus, UNINSTALL_HEAD: UNINSTALL_HEAD, uninstallHead: uninstallHead, savedMapsText: savedMapsText,
          SCOPE: SCOPE, MAP_LEVEL_TEXT: MAP_LEVEL_TEXT, create: create,
          NOTHING_PLACED_TEXT: NOTHING_PLACED_TEXT, EDITOR_TWEAKS_TEXT: EDITOR_TWEAKS_TEXT, NOTHING_CHANGED_TEXT: NOTHING_CHANGED_TEXT,
          changeLines: changeLines, slotText: slotText, pathParts: pathParts, editParts: editParts, valueClass: valueClass, folderArg: folderArg,
          PATH_TITLES: PATH_TITLES,
          FILE_CLASSES: FILE_CLASSES, FILE_SOURCES: FILE_SOURCES, NEW_MOD_HINT: NEW_MOD_HINT, normFiles: normFiles,
          fileTree: fileTree, fileMeta: fileMeta};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = modsTab;
