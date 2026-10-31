/* What a prefab adds to the SnapMap editor's limits. Pure (no DOM); browser global + CommonJS test
   export. Pure ASCII.

   The editor sums one 12-int block per counted thing (FIELDS below) and compares the sum with its
   limits. A placed entity costs exactly its palette row's sedef `limits` block, with network 0 when
   it is network-static; an entity whose inherit has no palette row costs 1 entity, 1 render model
   and 1 network. A placed module costs its own block, its template block, the VIEW-automation
   entities the engine instantiates into the room, and the 50 KB base of its entity bucket. The
   backend sends the per-row and per-module blocks once per palette build (backend/budget_table.c);
   this module only adds them up. */
var prefabBudget = (function () {
  var FIELDS = ['editorModels', 'entities', 'renderModels', 'lights', 'network', 'thinks', 'worldAreas',
                'decals', 'renderKB', 'collisionKB', 'generalKB', 'numAI'];
  var ENTITIES = 1, RENDER_MODELS = 2, LIGHTS = 3, NETWORK = 4, WORLD_AREAS = 6, DECALS = 7,
      RENDER_KB = 8, COLLISION_KB = 9, GENERAL_KB = 10, NUM_AI = 11;
  var BUCKET_BASE_KB = 50, AUTO_VIEW = 3;
  var TIMELINE_ROW = 'custom/timeline', TIMELINE_CLASS = 'idTarget_Timeline', UNKNOWN_INHERIT = 'snapmaps/unknown';
  var METERS = ['network', 'objects', 'memory', 'demons', 'decals'];
  var LABELS = {network:'NETWORK', objects:'OBJECTS', memory:'MEMORY', demons:'DEMONS', decals:'DECALS'};

  function zero() { return [0,0,0,0,0,0,0,0,0,0,0,0]; }
  function block(v) {
    var out = zero();
    if (!v || typeof v.length !== 'number') return out;
    for (var i = 0; i < 12; i++) { var n = Number(v[i]); out[i] = isFinite(n) ? n : 0; }
    return out;
  }
  function add(into, v) { for (var i = 0; i < 12; i++) into[i] += Number(v[i]) || 0; return into; }

  function leaf(name) {
    var s = String(name || ''), slash = s.lastIndexOf('/');
    if (slash >= 0) s = s.slice(slash + 1);
    return s.slice(-5) === '.decl' ? s.slice(0, -5) : s;
  }

  // A module's table entry by full decl name, by its leaf, or by a leaf with a numeric "N_" prefix.
  function findModule(table, name) {
    var mods = table && table.modules || {}, aliases = table && table.aliases || {};
    if (Object.prototype.hasOwnProperty.call(mods, name)) return mods[name];
    var l = leaf(name), candidates = [l, l.replace(/^\d+_/, '')];
    for (var i = 0; i < candidates.length; i++) {
      var full = aliases[candidates[i]];
      if (full && Object.prototype.hasOwnProperty.call(mods, full)) return mods[full];
    }
    return null;
  }

  // The palette row a scene entity binds to: the first row with its inherit, except a timeline,
  // which the product rebinds to its own tile.
  function rowFor(table, className, inherit) {
    var sedefs = table && table.sedefs || {}, inherits = table && table.inherits || {};
    if (inherit === UNKNOWN_INHERIT && className === TIMELINE_CLASS && sedefs[TIMELINE_ROW]) return sedefs[TIMELINE_ROW];
    var name = Object.prototype.hasOwnProperty.call(inherits, inherit) ? inherits[inherit] : null;
    return name && sedefs[name] ? sedefs[name] : null;
  }

  function entityParts(raw) {
    var def = raw && raw.entityDef ? raw.entityDef : (raw || {});
    var edit = def.state && def.state.edit ? def.state.edit : (raw && raw.edit) || {};
    return {className: typeof def.className === 'string' ? def.className : '',
            inherit: typeof def.inherit === 'string' ? def.inherit : '', edit: edit};
  }

  // One scene entity's block, or null for a palette miss.
  function entityBlock(table, raw) {
    var p = entityParts(raw), row = rowFor(table, p.className, p.inherit);
    if (!row) return null;
    var b = block(row.l);
    var isStatic = typeof p.edit.isNetworkStatic === 'boolean' ? p.edit.isNetworkStatic : !!row.ns;
    if (isStatic) b[NETWORK] = 0;
    return b;
  }

  // The editor's layer filter for a module bucket: the instance layerMask OR the snapLayers autoView
  // mask. Without the snapLayers decl the backend counts every layer, and so does this.
  function autoViewMask(table) {
    var l = table && table.layers;
    return l && l.source === 'decl' && isFinite(l.autoView) ? (l.autoView >>> 0) : 0xffffffff;
  }
  function overlaps(a, b) { return ((a >>> 0) & (b >>> 0)) !== 0; }
  function layerMaskOf(raw) {
    var m = raw && typeof raw.layerMask === 'number' ? raw.layerMask : 1;
    return m >>> 0;
  }

  /* A module's block: own + template + VIEW + the bucket's 50 KB. With a saved instance layerMask and the
     backend's per-mask groups, the template and VIEW parts are filtered by that mask, as the editor does;
     otherwise the backend's blocks, filtered with the palette default mask, are used. */
  function moduleBlock(entry, layerMask, table) {
    var b = add(zero(), block(entry.own));
    if (typeof layerMask === 'number' && Array.isArray(entry.parts)) {
      var filter = (layerMask | autoViewMask(table)) >>> 0;
      b[GENERAL_KB] += BUCKET_BASE_KB;   /* the template block's own base */
      for (var i = 0; i < entry.parts.length; i++) {
        var p = entry.parts[i];
        if (p && overlaps(p[0], filter)) add(b, block(p[2]));
      }
    } else add(add(b, block(entry.templ)), block(entry.view));
    b[GENERAL_KB] += BUCKET_BASE_KB;
    return b;
  }

  /* entities: scene entities (raw prefab JSON objects); modules: decl names or {name, layerMask}.
     opts.entityModule[i]: the module index entity i belongs to, -1 for the global group (counted on
     every layer); without it every entity is filtered as in a default room (layer 1 | autoView).
     opts.skipView: a module prefab's scene entity on a VIEW-automation row is not counted, because the
     engine instantiates those from the module itself (its view block), so each is counted once. */
  function sum(entities, modules, table, opts) {
    var total = zero(), misses = [], unknown = [], seenMiss = {}, seenUnknown = {}, filtered = 0;
    var list = Array.isArray(entities) ? entities : [], mods = Array.isArray(modules) ? modules : [];
    var skipView = !!(opts && opts.skipView), owner = opts && Array.isArray(opts.entityModule) ? opts.entityModule : null;
    var auto = autoViewMask(table);
    for (var i = 0; i < list.length; i++) {
      var pv = entityParts(list[i]), rv = rowFor(table, pv.className, pv.inherit);
      if (skipView && rv && rv.auto === AUTO_VIEW) continue;
      var filter = (1 | auto) >>> 0;
      if (owner) {
        var mi = owner[i], mod = mi >= 0 ? mods[mi] : null;
        if (mi < 0 || mi === undefined) filter = 0xffffffff;
        else if (mod && typeof mod.layerMask === 'number') filter = (mod.layerMask | auto) >>> 0;
      }
      if (!overlaps(layerMaskOf(list[i]), filter)) { filtered++; continue; }
      var b = entityBlock(table, list[i]);
      if (b) { add(total, b); continue; }
      total[ENTITIES] += 1; total[RENDER_MODELS] += 1; total[NETWORK] += 1;
      var key = pv.inherit || '(no inherit)';
      if (!seenMiss[key]) { seenMiss[key] = 1; misses.push(key); }
    }
    for (var m = 0; m < mods.length; m++) {
      var name = typeof mods[m] === 'string' ? mods[m] : (mods[m] && mods[m].name) || '';
      var entry = findModule(table, name);
      if (entry) add(total, moduleBlock(entry, mods[m] && typeof mods[m] === 'object' ? mods[m].layerMask : undefined, table));
      else if (!seenUnknown[name]) { seenUnknown[name] = 1; unknown.push(name); }
    }
    return {block: total, misses: misses, unknownModules: unknown, filtered: filtered};
  }

  /* What a module prefab file installs: its instances (name, saved layerMask) and, per scene entity,
     the instance whose instanceEntities group holds its uniqueId (-1 for the global group). */
  function moduleScene(doc) {
    var inst = doc && Array.isArray(doc.instances) ? doc.instances : [];
    var modules = inst.map(function (i) {
      return {name: (i && i.moduleName) || '', layerMask: i && typeof i.layerMask === 'number' ? i.layerMask : undefined};
    });
    var groups = doc && doc.instanceEntities || {}, keys = Array.isArray(groups.keyValues) ? groups.keyValues : [];
    var values = Array.isArray(groups.values) ? groups.values : [], byId = {};
    for (var k = 0; k + 1 < keys.length && k < modules.length; k++)
      for (var v = keys[k]; v < keys[k + 1] && v < values.length; v++) byId[values[v]] = k;
    var list = doc && Array.isArray(doc.entities) ? doc.entities : [];
    var owner = list.map(function (e) {
      var id = e && e.uniqueId;
      return Object.prototype.hasOwnProperty.call(byId, id) ? byId[id] : -1;
    });
    return {entities: list, modules: modules, entityModule: owner};
  }

  function ratio(n, d) { return d > 0 ? n / d : (n > 0 ? Infinity : 0); }

  // The five HUD meters for a usage block: the editor's own ratios (FUN_141183880).
  function meterRatios(u, l) {
    return {
      network: ratio(u[NETWORK], l[NETWORK]),
      objects: Math.max(ratio(u[ENTITIES], l[ENTITIES]),
                        ratio(u[RENDER_KB] + u[COLLISION_KB] + u[GENERAL_KB], l[RENDER_KB] + l[COLLISION_KB] + l[GENERAL_KB])),
      memory: Math.max(ratio(u[RENDER_MODELS], l[RENDER_MODELS]), ratio(u[LIGHTS], l[LIGHTS]),
                       ratio(u[WORLD_AREAS], l[WORLD_AREAS]), ratio(u[DECALS], l[DECALS])),
      demons: ratio(u[NUM_AI], l[NUM_AI]),
      decals: ratio(u[DECALS], l[DECALS])
    };
  }

  // "Limits reached": network, objects or memory at 1.0, or more than the demon limit.
  function reachedBy(r) { return r.network >= 1 || r.objects >= 1 || r.memory >= 1 || r.demons > 1; }
  function meterReached(name, value) { return name === 'demons' ? value > 1 : value >= 1; }

  /* The colour a meter's resulting value takes in Prefab Details: 'ok' below WARN_AT (75%), 'warn'
     from WARN_AT up to the limit, 'over' at or past it (100%). Every meter the editor would report as
     reached is therefore 'over'; DEMONS at exactly 100% is 'over' too, because no demon fits after it,
     although the editor's "limits reached" flag waits for the 65th. */
  var WARN_AT = 0.75, OVER_AT = 1;
  function meterLevel(r) {
    if (!(r < OVER_AT)) return isNaN(r) ? 'ok' : 'over';
    return r >= WARN_AT ? 'warn' : 'ok';
  }

  /* meters(addBlock, usage, limits[, held]). held: this prefab is the one the player is holding in
     the 3D view (a Load / Place before Space or Esc). The editor's usage already counts what is held
     (identical held and after Space, for object and module prefabs), so "Map after" is the live
     usage itself and the bar's current part is all of it: the prefab is never added twice. add
     stays the prefab's own share. */
  function meters(addBlock, usage, limits, held) {
    var l = block(limits), u = block(usage), after = held ? block(usage) : add(block(usage), addBlock);
    var rAdd = meterRatios(addBlock, l), rBefore = meterRatios(u, l), rAfter = meterRatios(after, l), out = {};
    for (var i = 0; i < METERS.length; i++) {
      var k = METERS[i];
      out[k] = {add: rAdd[k], before: rBefore[k], after: rAfter[k], reached: meterReached(k, rAfter[k]),
                level: meterLevel(rAfter[k]), held: !!held};
    }
    return {meters: out, reached: reachedBy(rAfter), after: after, held: !!held};
  }

  /* compute(entities, modules, table[, live[, opts]]) -> {block, meters, reached, misses, unknownModules}.
     live: {limits, usage} from the latest budgetUsage, else the table's own snapshot. */
  function compute(entities, modules, table, live, opts) {
    var s = sum(entities, modules, table || {}, opts);
    var limits = live && live.limits || table && table.limits, usage = live && live.usage || table && table.usage;
    var m = meters(s.block, usage, limits);
    return {block: s.block, meters: m.meters, reached: m.reached, misses: s.misses, unknownModules: s.unknownModules,
            filtered: s.filtered};
  }

  // A meter value as the HUD's percentage, one decimal; a nonzero amount below 0.05% reads "<0.1%".
  function formatPercent(r) {
    if (!isFinite(r)) return 'over';
    var pct = r * 100;
    if (pct > 0 && pct < 0.05) return '<0.1%';
    return pct.toFixed(1) + '%';
  }

  // The raw units behind each meter, for the row's tooltip.
  function rawText(name, b) {
    switch (name) {
      case 'network': return b[NETWORK] + ' network';
      case 'objects': return b[ENTITIES] + ' entities, ' + (b[RENDER_KB] + b[COLLISION_KB] + b[GENERAL_KB]) + ' KB memory';
      case 'memory': return b[RENDER_MODELS] + ' render models, ' + b[LIGHTS] + ' lights, ' + b[DECALS] + ' decals';
      case 'demons': return b[NUM_AI] + ' demons';
      case 'decals': return b[DECALS] + ' decals';
    }
    return '';
  }

  /* A bounded memo: get(key) / set(key, value), least recently used dropped first. */
  function createCache(limit) {
    var keys = [], values = {};
    limit = Math.max(1, limit | 0);
    return {
      get: function (k) {
        if (!Object.prototype.hasOwnProperty.call(values, k)) return undefined;
        keys.splice(keys.indexOf(k), 1); keys.push(k);
        return values[k];
      },
      set: function (k, v) {
        if (Object.prototype.hasOwnProperty.call(values, k)) keys.splice(keys.indexOf(k), 1);
        keys.push(k); values[k] = v;
        while (keys.length > limit) delete values[keys.shift()];
      },
      size: function () { return keys.length; },
      clear: function () { keys = []; values = {}; }
    };
  }

  return {FIELDS: FIELDS, METERS: METERS, LABELS: LABELS, BUCKET_BASE_KB: BUCKET_BASE_KB, WARN_AT: WARN_AT, OVER_AT: OVER_AT,
          meterLevel: meterLevel,
          compute: compute, sum: sum, meters: meters, meterRatios: meterRatios, reachedBy: reachedBy,
          entityBlock: entityBlock, moduleBlock: moduleBlock, moduleScene: moduleScene, findModule: findModule, leaf: leaf,
          formatPercent: formatPercent, rawText: rawText, createCache: createCache};
})();

if (typeof module !== 'undefined' && module.exports) module.exports = prefabBudget;
