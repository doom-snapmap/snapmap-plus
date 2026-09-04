"""Locate a function on a second build by signing one of its CALL SITES.

Some engine functions are not identifiable by their own bytes -- a tiny stub whose
body is a generic shape (`mov [rsp+0x20],r9; ret` plus padding) matches by accident,
not by identity. Scanning for such a body on a different build finds *a* function of
that shape, which is worse than finding nothing.

The fix is to sign the CALLER instead. A call site sits inside a large, distinctive
function, so it can be pinned uniquely; the call's rel32 then names the callee on
whatever build we are running. Same principle as derive_global.py, applied to a
`call rel32` displacement rather than a RIP-relative data displacement.
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM
from capstone.x86 import X86_REG_RIP, X86_GRP_JUMP, X86_GRP_CALL

from derive_global import sections, text_of, compile_pat, scan, wildcard_slots

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def call_sites(sec, target_rva):
    """Every `E8 rel32` whose destination is target_rva."""
    data, base = sec['data'], sec['va']
    out = []
    start = 0
    while True:
        k = data.find(b'\xE8', start)
        if k < 0 or k + 5 > len(data):
            break
        start = k + 1
        rel = struct.unpack_from('<i', data, k + 1)[0]
        if base + k + 5 + rel == target_rva:
            out.append(k)
    return out


def build_sig_from(sec, start_rva, n_insns, call_off):
    """Masked pattern of n_insns starting at start_rva; returns the byte offset of the
    rel32 belonging to the call at call_off."""
    data, base = sec['data'], sec['va']
    off = start_rva - base
    toks = []
    rel_slot = None
    cur = 0
    for ins in md.disasm(bytes(data[off:off + 16 * n_insns]), start_rva, count=n_insns):
        b = bytes(ins.bytes)
        masked = ['%02X' % x for x in b]
        for (s, ln) in wildcard_slots(ins):
            for k in range(s, s + ln):
                if k < len(masked):
                    masked[k] = '??'
        if ins.address == base + call_off and b and b[0] == 0xE8:
            rel_slot = cur + 1
            masked[1:5] = ['??'] * 4
        toks.extend(masked)
        cur += len(b)
    return toks, rel_slot


def derive(vk_secs, gl_secs, target_rva, max_sites=6):
    vt = text_of(vk_secs)
    sites = call_sites(vt, target_rva)
    results = []
    print('### VK callee %s -- %d direct call sites' % (hex(target_rva), len(sites)))
    for call_off in sites:
        if len(results) >= max_sites:
            break
        got = False
        # Walk the pattern window backwards from the call so the signature covers
        # distinctive surrounding code, not just the call itself.
        for back in (0, 1, 2, 3, 5, 8):
            if got:
                break
            # find an instruction boundary `back` instructions before the call
            start = call_off
            ok = True
            for _ in range(back):
                found = None
                for b in range(1, 16):
                    if start - b < 0:
                        continue
                    ins = list(md.disasm(bytes(vt['data'][start - b:start - b + 16]),
                                         vt['va'] + start - b, count=1))
                    if ins and ins[0].size == b:
                        found = start - b
                        break
                if found is None:
                    ok = False
                    break
                start = found
            if not ok:
                continue
            for n in (4, 6, 8, 11, 15):
                toks, rel_slot = build_sig_from(vt, vt['va'] + start, n, call_off)
                if rel_slot is None:
                    continue
                pb, pm = compile_pat(toks)
                vh = scan(vk_secs, pb, pm)
                if len(vh) != 1:
                    continue
                gh = scan(gl_secs, pb, pm)
                if len(gh) != 1:
                    continue
                gaddr, gsec, gp = gh[0]
                rel = struct.unpack_from('<i', gsec['data'], gp + rel_slot)[0]
                gl_target = gaddr + rel_slot + 4 + rel
                results.append((vt['va'] + call_off, gaddr, gl_target, len(toks),
                                ' '.join(toks), rel_slot))
                print('   vk call @%s -> gl call @%s  => GL callee %s  (%dB sig)'
                      % (hex(vt['va'] + call_off), hex(gaddr), hex(gl_target), len(toks)))
                got = True
                break
    agree = {}
    for r in results:
        agree[r[2]] = agree.get(r[2], 0) + 1
    if agree:
        best = max(agree.items(), key=lambda kv: kv[1])
        print('   CONSENSUS GL callee: %s  (%d/%d sites agree)'
              % (hex(best[0]), best[1], len(results)))
        print('   sig: %s' % results[0][4])
    else:
        print('   NO portable derivation found')
    return results


if __name__ == '__main__':
    import json, os
    vk_secs = sections(sys.argv[1])
    gl_secs = sections(sys.argv[2])
    out = []
    for t in sys.argv[3:]:
        target = int(t, 16)
        res = derive(vk_secs, gl_secs, target)
        tally = {}
        for r in res:
            tally[r[2]] = tally.get(r[2], 0) + 1
        rec = {'target_vk_rva': hex(target), 'ref_sites': len(res), 'derivations': []}
        if tally:
            consensus = max(tally.items(), key=lambda kv: kv[1])[0]
            for (vk_call, gl_call, gl_target, nbytes, pattern, rel_slot) in res:
                if gl_target != consensus:
                    continue
                rec['derivations'].append({
                    'vk_site': hex(vk_call),
                    # `pattern_start + rel_slot + 4 + rel32` is the callee -- the same
                    # arithmetic a RIP-relative data reference uses, so one resolver
                    # serves both. The pattern may begin before the call itself, so the
                    # slot is measured, not assumed.
                    'disp_slot': rel_slot,
                    'mnemonic': 'call rel32',
                    'bytes': nbytes,
                    'pattern': pattern,
                    'gl_site': hex(gl_call),
                    'gl_target_rva': hex(gl_target),
                })
        out.append(rec)
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'derived_callees.json')
    json.dump(out, open(p, 'w'), indent=1)
    print('\nwrote %s' % p)
