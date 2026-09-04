"""Derive a data global's RVA on a second DOOM build without hardcoding it.

Given a data RVA on the reference (Vulkan) image, find every RIP-relative code site
that computes that address, build a masked byte signature of the surrounding code
with volatile operands wildcarded (the same style as the product's shipped
signature DB), require the signature to be UNIQUE on both images, then read the
displacement at the matched site on the target (OpenGL) image to recover the
global's RVA there.

This is the mechanism that lets one shipped binary serve both builds: the product
signs a CODE SITE it can find on any build, and reads the data address out of it,
instead of baking an RVA that is only true for one build.
"""
import struct, sys, json, os
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM
from capstone.x86 import X86_REG_RIP, X86_GRP_JUMP, X86_GRP_CALL


def sections(path):
    d = open(path, 'rb').read()
    e = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, e + 6)[0]
    secoff = e + 24 + struct.unpack_from('<H', d, e + 20)[0]
    out = []
    for i in range(nsec):
        o = secoff + i * 40
        name = d[o:o + 8].rstrip(b'\x00').decode('latin1')
        vs, va, rs, ro = struct.unpack_from('<IIII', d, o + 8)
        chars = struct.unpack_from('<I', d, o + 36)[0]
        out.append(dict(name=name, va=va, vs=vs, ro=ro, rs=rs,
                        exec=bool(chars & 0x20000000), data=d[ro:ro + rs]))
    return out


def text_of(secs):
    for s in secs:
        if s['name'] == '.text':
            return s
    raise SystemExit('no .text')


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def riprefs(sec, target_rva):
    """Every offset where a 4-byte LE int equals target - (rva after those bytes).

    That is exactly the RIP-relative encoding rule, so each hit is a candidate
    reference site; the caller filters by actually decoding the instruction.
    """
    import numpy as np
    data, base = sec['data'], sec['va']
    b = np.frombuffer(data, dtype=np.uint8).astype(np.int64)
    n = len(b) - 4
    d32 = (b[0:n] | (b[1:n + 1] << 8) | (b[2:n + 2] << 16) | (b[3:n + 3] << 24))
    d32 = np.where(d32 >= 0x80000000, d32 - 0x100000000, d32)
    # target == base + off + 4 + d32  =>  d32 + off == target - base - 4
    # A reference whose disp32 is followed by an immediate encodes a correspondingly
    # smaller displacement, so sweep the plausible tail widths too. Missing these is how
    # a global with exactly one `cmp [rip+disp], imm8` reference looks unreferenced.
    idx = np.arange(n, dtype=np.int64)
    hits = []
    for tail in (0, 1, 2, 4):
        want = target_rva - base - 4 - tail
        hits.extend(np.nonzero(d32 + idx == want)[0].tolist())
    return sorted(set(hits))


def decode_site(sec, disp_off, back=24):
    """Find the instruction whose disp32 sits at disp_off."""
    data, base = sec['data'], sec['va']
    # Longest first: a decode that starts one byte later drops the REX prefix and
    # still parses (lea ecx vs lea rcx), which yields a weaker, misleading pattern.
    for b in range(back - 1, 0, -1):
        start = disp_off - b
        if start < 0:
            continue
        try:
            insns = list(md.disasm(bytes(data[start:start + 16]), base + start, count=1))
        except Exception:
            continue
        if not insns:
            continue
        ins = insns[0]
        for op in ins.operands:
            if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                enc = struct.pack('<i', op.mem.disp)
                idx = bytes(ins.bytes).rfind(enc)
                if idx >= 0 and start + idx == disp_off:
                    return ins
    return None


def wildcard_slots(ins):
    """Byte ranges to wildcard: RIP-relative displacements and branch rel32s.

    Small struct displacements stay FIXED. Struct layout is identical across the
    two builds; only addresses move, so keeping them raises selectivity for free.
    """
    slots = []
    b = bytes(ins.bytes)
    if (ins.group(X86_GRP_JUMP) or ins.group(X86_GRP_CALL)) and ins.size >= 5:
        if b[0] in (0xE8, 0xE9) or (b[0] == 0x0F and 0x80 <= b[1] <= 0x8F):
            slots.append((ins.size - 4, 4))
    for op in ins.operands:
        if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
            idx = b.rfind(struct.pack('<i', op.mem.disp))
            if idx >= 0:
                slots.append((idx, 4))
    return sorted(set(slots))


def build_sig(sec, anchor_ins, n_after=6):
    """Masked pattern starting at anchor_ins, covering n_after+1 instructions.

    Returns (tokens, start_rva, target_slot) where target_slot is the byte offset
    within the pattern of the disp32 that names our global.
    """
    data, base = sec['data'], sec['va']
    start_rva = anchor_ins.address
    off = start_rva - base
    toks = []
    target_slot = None
    target_tail = 0
    cur = 0
    for ins in md.disasm(bytes(data[off:off + 16 * (n_after + 1)]), start_rva, count=n_after + 1):
        b = bytes(ins.bytes)
        slots = wildcard_slots(ins)
        masked = ['%02X' % x for x in b]
        for (s, ln) in slots:
            for k in range(s, s + ln):
                if k < len(masked):
                    masked[k] = '??'
        if ins.address == start_rva and target_slot is None:
            for op in ins.operands:
                if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                    idx = b.rfind(struct.pack('<i', op.mem.disp))
                    # x86-64 measures the displacement from the end of the whole instruction,
                    # so anything following the disp32 (an immediate, as in
                    # `cmp dword ptr [rip+disp], 0`) has to be added back when decoding.
                    # Record how many such bytes there are rather than assuming none --
                    # assuming none decodes short, silently, onto a plausible address.
                    if idx >= 0:
                        target_slot = cur + idx
                        target_tail = len(b) - (idx + 4)
                        break
        toks.extend(masked)
        cur += len(b)
    return toks, start_rva, target_slot, target_tail


def compile_pat(toks):
    b = bytearray()
    m = bytearray()
    for t in toks:
        if t == '??':
            b.append(0)
            m.append(0)
        else:
            b.append(int(t, 16))
            m.append(0xFF)
    return bytes(b), bytes(m)


def scan(secs, pb, pm, limit=4):
    fixed = [i for i in range(len(pb)) if pm[i] == 0xFF]
    if not fixed:
        return []
    a0 = fixed[0]
    b0 = bytes([pb[a0]])
    hits = []
    n = len(pb)
    for s in secs:
        if not s['exec']:
            continue
        data = s['data']
        va = s['va']
        L = len(data)
        start = 0
        while True:
            k = data.find(b0, start)
            if k < 0:
                break
            start = k + 1
            p = k - a0
            if p < 0 or p + n > L:
                continue
            if all(data[p + i] == pb[i] for i in fixed):
                hits.append((va + p, s, p))
                if len(hits) > limit:
                    return hits
    return hits


def derive(vk_secs, gl_secs, target_rva, max_sites=6):
    vt = text_of(vk_secs)
    cand = riprefs(vt, target_rva)
    report = {'target_vk_rva': hex(target_rva), 'ref_sites': len(cand), 'derivations': []}
    for off in cand:
        if len(report['derivations']) >= max_sites:
            break
        ins = decode_site(vt, off)
        if ins is None:
            continue
        for n_after in (5, 7, 9, 12, 16):
            toks, srva, tslot, ttail = build_sig(vt, ins, n_after=n_after)
            if tslot is None:
                break
            pb, pm = compile_pat(toks)
            vh = scan(vk_secs, pb, pm)
            if len(vh) != 1:
                continue
            # Prove the site decodes back to the target we asked for. The tail sweep in riprefs
            # can surface a site whose real instruction length differs from the one assumed, and
            # such a site names a NEIGHBOURING global -- which for adjacent engine globals is a
            # plausible, wrong answer rather than an obvious failure.
            vaddr, vsec, vp = vh[0]
            d32v = struct.unpack_from('<i', vsec['data'], vp + tslot)[0]
            if vaddr + tslot + 4 + ttail + d32v != target_rva:
                continue
            gh = scan(gl_secs, pb, pm)
            if len(gh) != 1:
                continue
            gaddr, gsec, gp = gh[0]
            d32 = struct.unpack_from('<i', gsec['data'], gp + tslot)[0]
            gl_target = gaddr + tslot + 4 + ttail + d32
            report['derivations'].append({
                'vk_site': hex(srva),
                'disp_slot': tslot,
                'disp_tail': ttail,
                'mnemonic': '%s %s' % (ins.mnemonic, ins.op_str),
                'insns': n_after + 1,
                'bytes': len(toks),
                'pattern': ' '.join(toks),
                'gl_site': hex(gaddr),
                'gl_target_rva': hex(gl_target),
                'site_delta': hex(gaddr - srva),
            })
            break
    return report


if __name__ == '__main__':
    vk_secs = sections(sys.argv[1])
    gl_secs = sections(sys.argv[2])
    out = []
    for t in sys.argv[3:]:
        r = derive(vk_secs, gl_secs, int(t, 16))
        ds = r['derivations']
        print('\n### VK global %s -- %d rip-relative ref sites, %d portable derivations'
              % (r['target_vk_rva'], r['ref_sites'], len(ds)))
        agree = {}
        for d in ds:
            agree[d['gl_target_rva']] = agree.get(d['gl_target_rva'], 0) + 1
            print('   vk site %s (%s) -> gl site %s [%s]  => GL global %s  (%dB sig)'
                  % (d['vk_site'], d['mnemonic'], d['gl_site'], d['site_delta'],
                     d['gl_target_rva'], d['bytes']))
        if agree:
            best = max(agree.items(), key=lambda kv: kv[1])
            print('   CONSENSUS GL rva: %s  (%d/%d sites agree)' % (best[0], best[1], len(ds)))
            print('   sig: %s' % ds[0]['pattern'])
        out.append(r)
    json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'derived_globals.json'), 'w'), indent=1)
