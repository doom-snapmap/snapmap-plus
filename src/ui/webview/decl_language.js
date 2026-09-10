/* Declaration language services. Each instance owns its schema cache and has no DOM or host dependency. */
var SnapmapDecl = (function () {
  function create(schema) {
    var SCHEMA = schema || null;
    var rollupCache = {};

    // schema field specs are compact strings: "b" bool | "n:<t>" number | "s" string | "r:<t>" ref
    // (string-valued pointer) | "S:<struct>" nested block | "e:<enum>" | "A:<elem-spec>" array |
    // "o:<label>" unchecked
    function specArg(spec) { return spec.length > 2 ? spec.slice(2) : ''; }
    function specLabel(spec) {
      var k = spec[0], a = specArg(spec);
      if (k === 'A') return specLabel(a) + '[]';
      return k === 'b' ? 'bool' : k === 's' ? 'string' : (a || '?');
    }
    function unq(s) {
      if (s.charAt(0) === '"') s = s.slice(1);
      if (s.charAt(s.length - 1) === '"') s = s.slice(0, -1);
      return s;
    }
    function stripIdx(s) { return s.replace(/\[\d+\]$/, ''); }
    // runtime inheritance rollup: a struct's own fields merged over its parent chain (child wins).
    // The slice stores own-fields + a parent pointer; resolving here keeps the table ~4x smaller.
    function fieldsFor(structName) {
      if (!SCHEMA || !structName) return null;
      if (rollupCache.hasOwnProperty(structName)) return rollupCache[structName];
      var chain = [], seen = {}, cur = structName;
      while (cur && SCHEMA.structs[cur] && !seen[cur]) { seen[cur] = true; chain.push(SCHEMA.structs[cur]); cur = SCHEMA.structs[cur].p; }
      var merged = null;
      if (chain.length) {
        merged = {};
        for (var i = chain.length - 1; i >= 0; i--) { var f = chain[i].f; for (var k in f) merged[k] = f[k]; }
      }
      rollupCache[structName] = merged;
      return merged;
    }

    // -- tokenizer: comments, strings, numbers, identifiers (incl. list keys like item[0] and
    //    path idents like af/heavy), and the punctuation {}=;(),! the real decl corpus uses
    function tokenizeDecl(text) {
      var toks = [], i = 0, n = text.length, line = 0, m;
      function push(type, start, end, extra) {
        var tk = {type:type, start:start, end:end, line:line, text:text.slice(start,end)};
        if (extra) for (var k in extra) tk[k] = extra[k];
        toks.push(tk);
      }
      while (i < n) {
        var ch = text[i];
        if (ch === '\n') { line++; i++; continue; }
        if (ch === ' ' || ch === '\t' || ch === '\r') { i++; continue; }
        if (ch === '/' && text[i+1] === '/') { var c1 = i; while (i < n && text[i] !== '\n') i++; push('cmt', c1, i); continue; }
        if (ch === '/' && text[i+1] === '*') {
          var c2 = i, startLine = line; i += 2;
          while (i < n && !(text[i] === '*' && text[i+1] === '/')) { if (text[i] === '\n') line++; i++; }
          var closed = i < n; i = Math.min(n, i + 2);
          var save = line; line = startLine; push('cmt', c2, i, {unterminated: !closed}); line = save;
          continue;
        }
        if (ch === '"') {
          var s = i; i++;
          var term = false;
          while (i < n) {
            if (text[i] === '\\') { i += 2; continue; }
            if (text[i] === '"') { term = true; i++; break; }
            if (text[i] === '\n') break;
            i++;
          }
          push('str', s, i, {unterminated: !term});
          continue;
        }
        if (/[0-9]/.test(ch) || (ch === '-' && /[0-9.]/.test(text[i+1] || '')) || (ch === '.' && /[0-9]/.test(text[i+1] || ''))) {
          m = /^-?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?/.exec(text.slice(i));
          push('num', i, i + m[0].length); i += m[0].length; continue;
        }
        if (/[A-Za-z_#$]/.test(ch)) {
          var s2 = i; while (i < n && /[A-Za-z0-9_#$/.]/.test(text[i])) i++;
          // list keys serialize as name[N] -- absorb the index into the identifier
          if (text[i] === '[') { var j = i + 1; while (j < n && /[0-9]/.test(text[j])) j++; if (j > i + 1 && text[j] === ']') i = j + 1; }
          push('ident', s2, i); continue;
        }
        if ('{}=;(),!'.indexOf(ch) !== -1) { push('punc', i, i + 1); i++; continue; }
        push('bad', i, i + 1); i++;
      }
      return toks;
    }

    // Show contiguous indexed siblings in numeric order, not the engine's lexical key order.
    // Token spans ignore braces in strings/comments. Length-preserving replacements let
    // nested runs be normalized before parents without invalidating their offsets.
    function naturalizeIndexedDecl(text) {
      if (!text || text.indexOf('[') === -1) return text;
      var all = tokenizeDecl(text);
      var sig = [];
      for (var ai = 0; ai < all.length; ai++) if (all[ai].type !== 'cmt') sig.push(all[ai]);
      if (!sig.length) return text;

      var scopes = [{id:0, depth:0, open:-1, close:sig.length}];
      var scopeStack = [0], scopeOf = [], braceMate = {};
      for (var i = 0; i < sig.length; i++) {
        var token = sig[i];
        scopeOf[i] = scopeStack[scopeStack.length - 1];
        if (token.text === '{') {
          var child = {id:scopes.length, depth:scopeStack.length, open:i, close:-1};
          scopes.push(child);
          scopeStack.push(child.id);
        } else if (token.text === '}' && scopeStack.length > 1) {
          var childId = scopeStack.pop();
          scopes[childId].close = i;
          braceMate[scopes[childId].open] = i;
        }
      }

      var candidates = [];
      for (var ti = 0; ti < sig.length; ti++) {
        var keyToken = sig[ti];
        if (keyToken.type !== 'ident' && keyToken.type !== 'str') continue;
        var keyText = keyToken.text;
        if (keyToken.type === 'str' && keyText.length >= 2) keyText = keyText.slice(1, -1);
        var indexed = /^(.*)\[([0-9]+)\]$/.exec(keyText);
        if (!indexed || !indexed[1]) continue;

        var valueAt = ti + 1;
        if (!sig[valueAt] || (sig[valueAt].text !== '=' && sig[valueAt].text !== '{')) continue;
        if (sig[valueAt].text === '=') valueAt++;
        if (sig[valueAt] && sig[valueAt].text === '!') valueAt++;
        if (!sig[valueAt]) continue;

        var endSig = -1;
        if (sig[valueAt].text === '{') {
          if (braceMate[valueAt] === undefined) continue;
          endSig = braceMate[valueAt];
          if (sig[endSig + 1] && sig[endSig + 1].text === ';') endSig++;
        } else {
          for (var scan = valueAt; scan < sig.length; scan++) {
            if (scopeOf[scan] !== scopeOf[ti]) continue;
            if (sig[scan].text === ';') { endSig = scan; break; }
            if (sig[scan].text === '}') break;
          }
        }
        if (endSig < valueAt) continue;
        candidates.push({
          scope: scopeOf[ti],
          depth: scopes[scopeOf[ti]].depth,
          base: indexed[1],
          index: parseInt(indexed[2], 10),
          tokenAt: ti,
          endSig: endSig,
          start: keyToken.start,
          end: sig[endSig].end
        });
      }

      var lists = {};
      for (var ci = 0; ci < candidates.length; ci++) {
        var listKey = candidates[ci].scope + '\u0000' + candidates[ci].base;
        if (!lists[listKey]) lists[listKey] = [];
        lists[listKey].push(candidates[ci]);
      }
      var runs = [];
      for (var lk in lists) {
        var siblings = lists[lk], run = [];
        siblings.sort(function(a, b) { return a.tokenAt - b.tokenAt; });
        for (var li = 0; li <= siblings.length; li++) {
          var current = li < siblings.length ? siblings[li] : null;
          var previous = run.length ? run[run.length - 1] : null;
          if (!current || (previous && current.tokenAt !== previous.endSig + 1)) {
            if (run.length > 1) {
              var needsSort = false;
              for (var ri = 1; ri < run.length; ri++)
                if (run[ri - 1].index > run[ri].index) { needsSort = true; break; }
              if (needsSort) runs.push(run);
            }
            run = [];
          }
          if (current) run.push(current);
        }
      }
      if (!runs.length) return text;

      runs.sort(function(a, b) {
        return b[0].depth - a[0].depth || b[0].start - a[0].start;
      });
      var normalized = text;
      for (var r = 0; r < runs.length; r++) {
        var slots = runs[r], bodies = [], gaps = [];
        for (var si = 0; si < slots.length; si++) {
          bodies.push(normalized.slice(slots[si].start, slots[si].end));
          if (si + 1 < slots.length) gaps.push(normalized.slice(slots[si].end, slots[si + 1].start));
        }
        var order = [];
        for (var oi = 0; oi < slots.length; oi++) order.push(oi);
        order.sort(function(a, b) { return slots[a].index - slots[b].index || a - b; });
        var replacement = '';
        for (var pi = 0; pi < order.length; pi++) {
          replacement += bodies[order[pi]];
          if (pi < gaps.length) replacement += gaps[pi];
        }
        normalized = normalized.slice(0, slots[0].start) + replacement +
                     normalized.slice(slots[slots.length - 1].end);
      }
      return normalized;
    }

    // -- parser: entries of `key = value;` / `key { ... }`; value = { block } | ( tuple ) |
    //    scalar | bare multi-scalar (`size = 32 32 32;`) | `! { ... }` (override-block prefix).
    //    Wrappers (`entityDef <name> { ... }`, a bare outer `{ ... }`) parse as normal entries.
    function parseDecl(toks) {
      var diags = [];
      var sig = []; for (var i = 0; i < toks.length; i++) if (toks[i].type !== 'cmt') sig.push(toks[i]);
      var pos = 0;
      function peek(o) { return sig[pos + (o || 0)] || null; }
      function next() { return sig[pos++] || null; }
      function diag(tok, sev, msg) { if (tok) diags.push({tok: tok, sev: sev, msg: msg, cat: 'structural'}); }

      for (var k = 0; k < sig.length; k++) {
        var tk = sig[k], nx = sig[k + 1];
        if (tk.type === 'ident') {
          if (tk.text === 'true' || tk.text === 'false') tk.isBool = true;
          else if (nx && (nx.text === '=' || nx.text === '{')) tk.isKey = true;
        }
        // quoted keys exist too: `"bool" "" "zeroTransform" = true;` -- the last string is the key
        else if (tk.type === 'str' && nx && (nx.text === '=' || nx.text === '{')) tk.isKey = true;
      }
      for (var u = 0; u < toks.length; u++) {
        if (toks[u].unterminated) diag(toks[u], 'err', toks[u].type === 'str' ? 'Unterminated string.' : "Unterminated '/*' comment.");
        if (toks[u].type === 'bad') diag(toks[u], 'err', "Unexpected character '" + toks[u].text + "'.");
      }

      function isScalarTok(t) { return t && (t.type === 'num' || t.type === 'str' || t.type === 'ident'); }
      function parseTuple(open) {
        var items = [];
        while (true) {
          var t = peek();
          // stop (without consuming) at a token that clearly belongs to the enclosing statement,
          // so one unclosed '(' doesn't cascade into unrelated diagnostics
          if (!t || t.text === '}' || t.text === ';') { diag(open, 'err', "Unclosed '(' -- missing ')'."); return {kind:'tuple', items:items, closed:false}; }
          if (t.text === ')') { next(); return {kind:'tuple', items:items, closed:true}; }
          if (t.text === ',') { next(); continue; }
          if (t.text === '(') { next(); var inner = parseTuple(t); items = items.concat(inner.items); continue; }
          if (isScalarTok(t)) { items.push(next()); continue; }
          diag(next(), 'err', "Unexpected '" + t.text + "' inside ( ... ).");
        }
      }
      function parseValue(keyTok, depth) {
        var v = peek();
        if (v && v.text === '!') { next(); v = peek(); }   // override-block prefix: `key = ! { ... }`
        if (!v) { diag(keyTok, 'err', "Missing value for '" + keyTok.text + "'."); return null; }
        if (v.text === '{') {
          next();
          var blk = parseEntries(depth + 1);
          if (!blk.closed) diag(v, 'err', "Unclosed '{' -- missing '}'.");
          if (peek() && peek().text === ';') next();   // ';' after '}' is optional
          return {kind:'block', entries: blk.entries};
        }
        if (v.text === '(') {
          next();
          var tp = parseTuple(v);
          if (peek() && peek().text === ';') next();
          else if (tp.closed) diag(v, 'err', "Missing ';' after ( ... ).");
          return tp;
        }
        if (isScalarTok(v)) {
          next();
          // bare multi-scalar: `size = 32 32 32;` -- absorb following non-key scalars into a tuple
          var items = [v];
          while (isScalarTok(peek()) && !peek().isKey) items.push(next());
          if (peek() && peek().text === ';') next();
          else diag(items[items.length - 1], 'err', "Missing ';' after this value.");
          return items.length > 1 ? {kind:'tuple', items: items} : {kind:'scalar', val: v};
        }
        diag(v, 'err', "Unexpected '" + v.text + "' after '" + keyTok.text + "'.");
        next();
        return null;
      }
      function parseEntries(depth) {
        var entries = [];
        while (true) {
          var t = peek();
          if (!t) return {entries: entries, closed: depth === 0};
          if (t.text === '}') {
            if (depth === 0) { diag(next(), 'err', "Unmatched '}'."); continue; }
            next(); return {entries: entries, closed: true};
          }
          if (t.text === '{') {   // anonymous block (a decl file's outer { ... } wrapper)
            var open = next();
            var blk = parseEntries(depth + 1);
            if (!blk.closed) diag(open, 'err', "Unclosed '{' -- missing '}'.");
            entries = entries.concat(blk.entries);
            continue;
          }
          if (t.type === 'str' && !t.isKey) { next(); continue; }   // stray annotation strings ("idReferenceMap" "" edit = {) -- shipped decls carry them
          if ((t.type === 'ident' && !t.isBool) || (t.type === 'str' && t.isKey)) {
            var key = next();
            // `entityDef <name> { ... }` -- the name token sits between the key and its block
            if (peek() && (peek().type === 'ident' || peek().type === 'str') && peek(1) && peek(1).text === '{') next();
            var nx2 = peek();
            if (nx2 && (nx2.text === '=' || nx2.text === '{')) {
              if (nx2.text === '=') next();
              var val = parseValue(key, depth);
              if (val) entries.push({key: key, kind: val.kind, entries: val.entries, items: val.items, val: val.val});
              continue;
            }
            diag(key, 'err', "Expected '=' after '" + key.text + "'.");
            // recover to the end of this statement so one typo doesn't cascade into noise
            while (peek() && peek().text !== ';' && peek().text !== '}') next();
            if (peek() && peek().text === ';') next();
            continue;
          }
          diag(next(), 'err', "Unexpected '" + t.text + "' -- expected a field name" + (depth ? " or '}'" : '') + '.');
        }
      }
      var root = parseEntries(0);
      return {rootEntries: root.entries, diags: diags};
    }

    // Schema diagnostics are advisory because the table is build-derived. Engine Save remains authoritative.
    var SCHEMA_SEV = { 'unknown-field': 'warn', 'type': 'warn', 'enum-value': 'warn', 'top': 'warn' };
    var DECL_META_KEYS = { inherit:1, 'class':1, expandInheritance:1, poolCount:1, poolGranularity:1,
                           editorVars:1, systemVars:1, entity:1, entityDef:1 };
    function kindWord(spec) {
      var k = spec[0];
      return k === 'b' ? 'true or false' : k === 'n' ? 'a number (' + specArg(spec) + ')'
           : k === 's' ? 'a string' : k === 'r' ? 'a name/path (' + specArg(spec) + ')'
           : k === 'e' ? 'a ' + specArg(spec) + ' value' : 'a value';
    }
    function sdiag(diags, tok, cat, msg) { diags.push({tok: tok, sev: SCHEMA_SEV[cat] || 'warn', cat: cat, msg: msg}); }
    function checkEntry(e, spec, owner, diags) {
      var k = spec[0];
      if (k === 'o') return;   // lists/templates/unmapped types: unchecked by design
      if (k === 'A') {
        var elem = specArg(spec);
        if (e.kind === 'block') {
          // array wrapper block: `mat = { mat[0] = {...}; mat[1] = {...}; }` -- check indexed rows
          // against the element spec; tolerate serializer bookkeeping keys (num etc.)
          var base = stripIdx(unq(e.key.text));
          for (var j = 0; j < e.entries.length; j++) {
            var se = e.entries[j], sk = unq(se.key.text);
            if (stripIdx(sk) === base && sk !== base) checkEntry(se, elem, owner, diags);
          }
        } else if (e.kind === 'tuple') {
          if (elem[0] === 'n') {
            for (var m = 0; m < e.items.length; m++)
              if (e.items[m].type !== 'num') sdiag(diags, e.items[m], 'type', "'" + e.key.text + "' ( ... ) components should be numbers.");
          }
        } else if (e.kind === 'scalar') {
          checkEntry({key: e.key, kind: 'scalar', val: e.val}, elem, owner, diags);
        }
        return;
      }
      if (k === 'S') {
        if (e.kind === 'block') { var sub = fieldsFor(specArg(spec)); if (sub) checkBlock(e.entries, sub, specArg(spec), diags); }
        else if (e.kind === 'tuple') {
          for (var j2 = 0; j2 < e.items.length; j2++)
            if (e.items[j2].type !== 'num') sdiag(diags, e.items[j2], 'type', "'" + e.key.text + "' ( ... ) components should be numbers (" + specArg(spec) + ').');
        }
        else sdiag(diags, e.key, 'type', "'" + e.key.text + "' expects a { ... } block or ( ... ) tuple (" + specArg(spec) + ').');
        return;
      }
      if (e.kind === 'block' || e.kind === 'tuple') { sdiag(diags, e.key, 'type', "'" + e.key.text + "' expects " + kindWord(spec) + '.'); return; }
      var v = e.val;
      if (k === 'b') {
        var boolOk = (v.type === 'ident' && v.isBool) || (v.type === 'num' && (v.text === '0' || v.text === '1'));
        if (!boolOk) sdiag(diags, v, 'type', "'" + e.key.text + "' expects true or false (bool).");
      }
      else if (k === 'n' && v.type !== 'num') sdiag(diags, v, 'type', "'" + e.key.text + "' expects a number (" + specArg(spec) + ').');
      else if ((k === 's' || k === 'r') && !(v.type === 'str' || v.type === 'ident')) sdiag(diags, v, 'type', "'" + e.key.text + "' expects " + kindWord(spec) + '.');
      else if (k === 'e' && v.type !== 'num') {
        var vals = SCHEMA.enums[specArg(spec)];
        if (vals && vals.length) {
          // flag enums serialize as ONE string of space-separated constants -- check each token
          var parts = unq(v.text).split(/\s+/).filter(Boolean);
          for (var q = 0; q < parts.length; q++)
            if (vals.indexOf(parts[q]) === -1)
              sdiag(diags, v, 'enum-value', "'" + parts[q] + "' is not among the known " + specArg(spec) + ' values (' + vals.slice(0, 4).join(', ') + (vals.length > 4 ? ', ...' : '') + ') -- double-check.');
        }
      }
    }
    function checkBlock(entries, table, owner, diags, suppressUnknown) {
      for (var i = 0; i < entries.length; i++) {
        var e = entries[i], keyName = unq(e.key.text), spec = table[keyName];
        if (!spec) {
          // inline array rows: `additiveLag[0] = {...}` looks up its base name's element spec
          var base = stripIdx(keyName);
          if (base !== keyName && table[base]) { var as = table[base]; spec = as[0] === 'A' ? specArg(as) : as; }
        }
        if (!spec) {
          // engine-attested deserializer aliases (audit-observed in shipped game data) don't warn
          var isAlias = SCHEMA.aliases && SCHEMA.aliases.indexOf(stripIdx(keyName)) !== -1;
          if (!suppressUnknown && !isAlias) sdiag(diags, e.key, 'unknown-field', "'" + keyName + "' is not a known field of " + owner + '.');
          continue;
        }
        checkEntry(e, spec, owner, diags);
      }
    }
    // Returns 'exact' (class in the schema), 'base' (unknown class -> checked against the idEntity
    // base fields, unknown-key warnings suppressed since we can't know its own fields), or false.
    function schemaCheck(rootEntries, className, diags) {
      var mode = 'exact';
      var clsFields = fieldsFor(className);
      if (!clsFields) { clsFields = fieldsFor('idEntity'); mode = clsFields ? 'base' : false; }
      function walk(entries) {
        for (var i = 0; i < entries.length; i++) {
          var e = entries[i], key = unq(e.key.text);
          if ((key === 'entity' || key === 'entityDef') && e.kind === 'block') { walk(e.entries); continue; }
          if (key === 'edit') {
            if (e.kind !== 'block') diags.push({tok: e.key, sev: 'err', cat: 'structural', msg: "'edit' expects a { ... } block."});
            else if (clsFields) checkBlock(e.entries, clsFields, mode === 'base' ? 'idEntity (base)' : className, diags, mode === 'base');
            continue;
          }
          if (DECL_META_KEYS[key]) continue;
          sdiag(diags, e.key, 'top', "'" + key + "' -- unexpected top-level key (expected inherit / class / edit / editorVars ...).");
        }
      }
      walk(rootEntries);
      return mode;
    }

    function esc(s) { return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'); }
    // -- overlay renderer: colored tokens + squiggles into the <pre> behind the textarea --
    function renderHl(text, toks, diags) {
      var byStart = {};
      for (var i = 0; i < diags.length; i++) {
        var d = diags[i], id = d.tok.start;
        if (!byStart[id] || (d.sev === 'err' && byStart[id].sev !== 'err')) byStart[id] = d;
      }
      var h = '', last = 0;
      for (var j = 0; j < toks.length; j++) {
        var tk = toks[j];
        h += esc(text.slice(last, tk.start));
        var cls = tk.type === 'cmt' ? 'tk-cmt'
                : tk.type === 'str' ? 'tk-str'
                : tk.type === 'num' ? 'tk-num'
                : tk.isBool ? 'tk-bool'
                : tk.isKey ? 'tk-key'
                : tk.type === 'ident' ? 'tk-ident'
                : tk.type === 'bad' ? 'tk-bad' : 'tk-punc';
        var dg = byStart[tk.start];
        if (dg) cls += dg.sev === 'err' ? ' sq-err' : ' sq-warn';
        h += '<span class="' + cls + '">' + esc(tk.text) + '</span>';
        last = tk.end;
      }
      h += esc(text.slice(last)) + '\n';
      return h;
    }

    function acContext(declTokens, cur) {
      // walk the significant tokens before the cursor, tracking the {key -> block} nesting stack and
      // whether the cursor sits in key position or in value position (after 'key =')
      var stack = [], pendingKey = null, lastIdent = null, state = 'key', touched = null;
      for (var i = 0; i < declTokens.length; i++) {
        var tk = declTokens[i];
        if (tk.type === 'cmt') continue;
        if (tk.start >= cur) break;
        if (tk.end >= cur && (tk.type === 'ident' || tk.type === 'num')) { touched = tk; break; }
        if (tk.type === 'ident' && !tk.isBool) lastIdent = tk;
        if (tk.text === '=') { pendingKey = lastIdent; state = 'value'; }
        else if (tk.text === '{') { stack.push(pendingKey ? pendingKey.text : null); pendingKey = null; state = 'key'; }
        else if (tk.text === '}') { stack.pop(); pendingKey = null; state = 'key'; }
        else if (tk.text === ';') { pendingKey = null; state = 'key'; }
        else if (state === 'value' && (tk.type === 'str' || tk.type === 'num' || tk.isBool)) state = 'done';
      }
      var prefix = '', replaceFrom = cur;
      if (touched) { prefix = touched.text.slice(0, cur - touched.start); replaceFrom = touched.start; }
      return {stack: stack, state: state, pendingKey: pendingKey, prefix: prefix, replaceFrom: replaceFrom};
    }
    function tableForStack(stack, cls) {
      if (!SCHEMA) return null;
      if (stack.length === 0) return {edit: 'o:the editable block'};
      // tolerate the entity/entityDef wrapper levels a full decl blob carries
      while (stack.length && (stack[0] === 'entity' || stack[0] === 'entityDef' || stack[0] === null)) stack = stack.slice(1);
      if (!stack.length || stack[0] !== 'edit') return null;
      var table = fieldsFor(cls) || fieldsFor('idEntity');   // unknown class -> base-field completion
      if (!table) return null;
      for (var i = 1; i < stack.length; i++) {
        var key = stripIdx(unq(stack[i]));
        var spec = table[key];
        if (spec && spec[0] === 'A') spec = specArg(spec);   // array rows complete as their element type
        if (!spec || spec[0] !== 'S') return null;
        table = fieldsFor(specArg(spec));
        if (!table) return null;
      }
      return table;
    }
    function complete(declTokens, cursor, className, forced) {
      var ctx = acContext(declTokens, cursor);
      var out = [];
      if (ctx.state === 'key') {
        var table = tableForStack(ctx.stack, className);
        if (table) for (var k in table) {
          if (!ctx.prefix || k.toLowerCase().indexOf(ctx.prefix.toLowerCase()) === 0)
            out.push({label: k, type: specLabel(table[k])});
        }
        out.sort(function(a, b){ return a.label < b.label ? -1 : 1; });
      } else if (ctx.state === 'value' && ctx.pendingKey) {
        var tbl = tableForStack(ctx.stack, className);
        var spec = tbl && tbl[ctx.pendingKey.text];
        if (spec && spec[0] === 'b') {
          ['true', 'false'].forEach(function(v){ if (!ctx.prefix || v.indexOf(ctx.prefix) === 0) out.push({label: v, type: 'bool'}); });
        } else if (spec && spec[0] === 'e') {
          var vals = SCHEMA.enums[specArg(spec)] || [];
          var pq = unq(ctx.prefix);
          vals.forEach(function(v){ if (!pq || v.toLowerCase().indexOf(pq.toLowerCase()) === 0) out.push({label: '"' + v + '"', type: specArg(spec)}); });
        }
      }
      if (!forced && !ctx.prefix) return {items: [], from: ctx.replaceFrom};   // only pop up unprompted once the user is typing a name
      return {items: out.slice(0, 60), from: ctx.replaceFrom};
    }
    return {
      tokenize: tokenizeDecl,
      parse: parseDecl,
      checkSchema: schemaCheck,
      naturalize: naturalizeIndexedDecl,
      highlight: renderHl,
      complete: complete
    };
  }
  return {create: create};
}());
if (typeof module !== 'undefined' && module.exports) module.exports = SnapmapDecl;
