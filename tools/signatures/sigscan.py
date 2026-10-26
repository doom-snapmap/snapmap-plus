"""Scan every engine byte pattern Snapmap+ ships against DOOM executables.

Mirrors the native resolver (src/backend/signatures.c, engine_globals.c): masked
patterns are matched over every executable section, the longest fixed run is the
anchor, and only a unique match counts.

Snapmap+ supports one build, the April 2024 update (Steam build 13954591). Its two
executables (identified by the SHA-256 table in src/backend/host_image.c) must give
a unique match for every function pattern at the RVA recorded for that renderer, and
every global anchor must decode to its recorded RVA. Any other image -- for example
the 2018 executables -- is scanned as an optional cross-check: uniqueness is
reported, but it does not affect the exit status.

Covered: every BACKEND_ENGINE_SIGNATURES row, the OpenGL-only helper, the
fault-shield table, the standalone pattern constants, and every global anchor in
engine_globals_table.gen.h.

    python sigscan.py <image> [<image> ...] [--src <product root>] [--json <out.json>]

The native suite tests/run-tests.ps1 -Doom <image> -DoomAlt <image> remains the
required gate.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def strip_comments(s):
    out = []
    i, n = 0, len(s)
    while i < n:
        if s.startswith('/*', i):
            j = s.find('*/', i + 2)
            i = (j + 2) if j >= 0 else n
        elif s.startswith('//', i):
            j = s.find('\n', i)
            i = j if j >= 0 else n
        elif s[i] == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            out.append(s[i:j + 1])
            i = j + 1
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


STR = r'((?:"[^"]*"\s*)+)'
NUM = r'(0x[0-9A-Fa-f]+|\d+)u?'


def cat(strings):
    return ''.join(re.findall(r'"([^"]*)"', strings))


def parse_table(path, symbol):
    """Rows of a NULL-terminated sig_entry array: (name, pattern, rva_vk, rva_gl)."""
    src = open(path, encoding='utf-8', errors='replace').read()
    head = re.search(re.escape(symbol) + r'\[\]\s*=\s*\{', src)
    if not head:
        raise SystemExit('%s: table %s not found' % (path, symbol))
    body = strip_comments(src[head.end():].split('\n};', 1)[0])
    row = re.compile(r'\{\s*' + STR + r',\s*' + STR + r',\s*' + NUM + r',\s*' + NUM + r'\s*,?\s*\}')
    rows = [(cat(m.group(1)), cat(m.group(2)), int(m.group(3), 0), int(m.group(4), 0))
            for m in row.finditer(body)]
    declared = len(re.findall(r'\{\s*"', body))
    if len(rows) != declared:
        raise SystemExit('%s: parsed %d of %d %s rows; refusing a partial scan'
                         % (path, len(rows), declared, symbol))
    return rows


def parse_constant_entry(path, symbol):
    """A single `const sig_entry SYMBOL = { "name", "pattern", rva_vk, rva_gl };`."""
    src = strip_comments(open(path, encoding='utf-8', errors='replace').read())
    m = re.search(re.escape(symbol) + r'\s*=\s*\{\s*' + STR + r',\s*' + STR + r',\s*' + NUM + r',\s*' + NUM, src)
    if not m:
        raise SystemExit('%s: %s not found' % (path, symbol))
    return (cat(m.group(1)), cat(m.group(2)), int(m.group(3), 0), int(m.group(4), 0))


def parse_string_constant(path, symbol):
    """A pattern held in `#define SYMBOL "..." "..."` or `... SYMBOL = "..." "...";`."""
    src = open(path, encoding='utf-8', errors='replace').read()
    m = (re.search(r'#define\s+' + re.escape(symbol) + r'((?:\s*\\?\s*"[^"]*")+)', src) or
         re.search(re.escape(symbol) + r'\s*=\s*((?:\s*"[^"]*")+)\s*;', src))
    if not m:
        raise SystemExit('%s: pattern constant %s not found' % (path, symbol))
    return (symbol, cat(m.group(1)), 0, 0)


def parse_globals(path):
    """Anchors and recorded RVAs from engine_globals_table.gen.h."""
    src = open(path, encoding='utf-8', errors='replace').read()
    rows = []
    for m in re.finditer(r'\{ \.name = "([^"]+)",.*?\.anchor = "([^"]*)",\s*'
                         r'\.disp_slot = (\d+), \.disp_tail = (\d+), \.delta = (-?\d+), '
                         r'\.rva_vk = 0x([0-9A-Fa-f]+)u, \.rva_gl = 0x([0-9A-Fa-f]+)u \}', src, re.S):
        rows.append(dict(name=m.group(1), anchor=m.group(2), slot=int(m.group(3)),
                         tail=int(m.group(4)), delta=int(m.group(5)),
                         vk=int(m.group(6), 16), gl=int(m.group(7), 16)))
    declared = len(re.findall(r'\{ \.name = "', src))
    if len(rows) != declared:
        raise SystemExit('%s: parsed %d of %d globals; refusing a partial scan'
                         % (path, len(rows), declared))
    return rows


def parse_known_builds(path):
    """SH_KNOWN_BUILDS from host_image.c: sha256 hex -> (vulkan, label)."""
    src = open(path, encoding='utf-8', errors='replace').read()
    table = src.split('SH_KNOWN_BUILDS[] = {', 1)[1].split('\n};', 1)[0]
    builds = {}
    for m in re.finditer(r'\{\{([0-9a-fx,\s]+)\},\s*(\d),\s*"([^"]+)"\}', table):
        digest = bytes(int(b, 16) for b in m.group(1).split(','))
        builds[digest.hex()] = (int(m.group(2)), m.group(3))
    if not builds:
        raise SystemExit('%s: SH_KNOWN_BUILDS not parsed' % path)
    return builds


def compile_pat(pat):
    toks = pat.split()
    if not toks:
        return None, None
    b, mask = bytearray(), bytearray()
    for t in toks:
        if t in ('??', '?'):
            b.append(0)
            mask.append(0)
        else:
            b.append(int(t, 16))
            mask.append(1)
    return bytes(b), bytes(mask)


class Image:
    def __init__(self, path):
        self.path = path
        data = open(path, 'rb').read()
        self.sha256 = hashlib.sha256(data).hexdigest()
        e = struct.unpack_from('<I', data, 0x3c)[0]
        nsec = struct.unpack_from('<H', data, e + 6)[0]
        opt = e + 24
        self.size_of_image = struct.unpack_from('<I', data, opt + 56)[0]
        secoff = opt + struct.unpack_from('<H', data, e + 20)[0]
        self.secs = []
        for i in range(nsec):
            o = secoff + i * 40
            vs, va, rs, ro = struct.unpack_from('<IIII', data, o + 8)
            chars = struct.unpack_from('<I', data, o + 36)[0]
            if chars & 0x20000000:   # IMAGE_SCN_MEM_EXECUTE
                self.secs.append((va, data[ro:ro + rs]))

    def rva_hits(self, pattern, limit=2):
        pb, pm = compile_pat(pattern)
        if pb is None:
            return None
        n = len(pb)
        best_off = best_len = cur = run = 0
        for i in range(n):
            if pm[i]:
                if run == 0:
                    cur = i
                run += 1
                if run > best_len:
                    best_len, best_off = run, cur
            else:
                run = 0
        if not best_len:
            return None
        anchor = pb[best_off:best_off + best_len]
        fixed = [i for i in range(n) if pm[i]]
        hits = []
        for va, data in self.secs:
            pos = best_off
            while len(hits) < limit:
                k = data.find(anchor, pos)
                if k < 0:
                    break
                pos = k + 1
                s = k - best_off
                if s < 0 or s + n > len(data):
                    continue
                if all(data[s + i] == pb[i] for i in fixed):
                    hits.append(va + s)
        return hits

    def read_i32(self, rva):
        for va, data in self.secs:
            if va <= rva and rva + 4 <= va + len(data):
                return struct.unpack_from('<i', data, rva - va)[0]
        return None


def status_of(hits):
    if hits is None:
        return 'BAD_PATTERN'
    return 'OK' if len(hits) == 1 else ('NOT_FOUND' if not hits else 'AMBIGUOUS')


def scan_image(img, build, sig_rows, globals_rows):
    supported = build is not None
    is_gl = (build[0] == 0) if build else 'DOOMx64.exe' in os.path.basename(img.path)
    rows = []
    for table, name, pattern, rva_vk, rva_gl in sig_rows:
        known = (rva_gl if is_gl else rva_vk) if supported else 0
        hits = img.rva_hits(pattern)
        row = dict(table=table, name=name, known_rva=hex(known), hits=[hex(h) for h in hits or []])
        if name == 'NavRenderTargetGL' and not is_gl:
            # OpenGL-only helper: it must be absent from Vulkan images.
            row['status'] = 'OK' if hits == [] else 'PRESENT_ON_VULKAN'
        else:
            row['status'] = status_of(hits)
            if row['status'] == 'OK' and known and hits[0] != known:
                row['status'] = 'RVA_MISMATCH'
        rows.append(row)

    grows = []
    for g in globals_rows:
        recorded = (g['gl'] if is_gl else g['vk']) if supported else 0
        result = dict(name=g['name'], recorded_rva=hex(recorded), rva=None)
        hits = img.rva_hits(g['anchor'])
        result['status'] = status_of(hits)
        if result['status'] == 'OK':
            disp = img.read_i32(hits[0] + g['slot'])
            rva = None if disp is None else hits[0] + g['slot'] + 4 + g['tail'] + disp + g['delta']
            if rva is not None and 0 <= rva < img.size_of_image:
                result['rva'] = hex(rva)
                if recorded and rva != recorded:
                    result['status'] = 'RVA_MISMATCH'
            else:
                result['status'] = 'OUT_OF_RANGE'
        grows.append(result)
    return rows, grows


def load_sources(root):
    backend = os.path.join(root, 'src', 'backend')
    shield = os.path.join(root, 'src', 'fault_shield')
    sig_rows = [('backend',) + r for r in parse_table(os.path.join(backend, 'signatures.c'),
                                                     'BACKEND_ENGINE_SIGNATURES')]
    sig_rows.append(('backend',) + parse_constant_entry(os.path.join(backend, 'signatures.c'),
                                                        'NAV_RENDER_TARGET_GL_SIGNATURE'))
    sig_rows += [('shield',) + r for r in parse_table(os.path.join(shield, 'shield_sigs.c'),
                                                     'SHIELD_ENGINE_SIGNATURES')]
    sig_rows.append(('constant',) + parse_string_constant(os.path.join(backend, 'cvar_unlock.c'),
                                                          'CVU_CMDSYS_LEA_SIG'))
    sig_rows.append(('constant',) + parse_string_constant(os.path.join(backend, 'decl_visibility.c'),
                                                          'DV_PROBE_SIGNATURE'))
    globals_rows = parse_globals(os.path.join(backend, 'engine_globals_table.gen.h'))
    builds = parse_known_builds(os.path.join(backend, 'host_image.c'))
    return sig_rows, globals_rows, builds


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('images', nargs='+')
    ap.add_argument('--src', default=os.path.normpath(os.path.join(HERE, '..', '..')),
                    help='product repository root (default: this checkout)')
    ap.add_argument('--json', help='write the per-image results here (parent directory is created)')
    args = ap.parse_args(argv)
    sig_rows, globals_rows, builds = load_sources(args.src)

    results = {}
    failed = 0
    for path in args.images:
        img = Image(path)
        build = builds.get(img.sha256)
        rows, grows = scan_image(img, build, sig_rows, globals_rows)
        label = build[1] if build else 'not a supported executable (cross-check only)'
        bad = [r for r in rows + grows if r['status'] != 'OK']
        if build:
            failed += len(bad)
        results[path] = dict(sha256=img.sha256, build=label, signatures=rows, globals=grows)
        print('\n=== %s [%s] ===' % (path, label))
        print('signatures: %d/%d OK   globals: %d/%d OK'
              % (sum(r['status'] == 'OK' for r in rows), len(rows),
                 sum(r['status'] == 'OK' for r in grows), len(grows)))
        for r in bad:
            print('  %-17s %-9s %-30s hits=%s recorded=%s' % (
                r['status'], r.get('table', 'global'), r['name'], r.get('hits', r.get('rva')),
                r.get('known_rva', r.get('recorded_rva'))))
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, 'w') as f:
            json.dump(results, f, indent=1)
        print('\nwrote %s' % args.json)
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
