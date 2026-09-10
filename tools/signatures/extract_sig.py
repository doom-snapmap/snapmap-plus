"""Grow a masked function signature until it matches uniquely on the reference
image, then require uniqueness on the target. Mask relative addresses while
retaining structural operands. A unique match still needs semantic verification.
Usage: extract_sig.py <ref.exe> <target.exe> <rva> [<rva> ...]"""
import struct, sys

from derive_global import sections, text_of, compile_pat, scan, wildcard_slots, md


def extract(ref_secs, tgt_secs, rva, max_insns=40):
    vt = text_of(ref_secs)
    off = rva - vt['va']
    if off < 0 or off >= len(vt['data']):
        return {'rva': rva, 'error': 'rva outside .text'}
    blob = bytes(vt['data'][off:off + 16 * max_insns])
    toks = []
    n_insns = 0
    for ins in md.disasm(blob, rva, count=max_insns):
        b = bytes(ins.bytes)
        masked = ['%02X' % x for x in b]
        for (s, ln) in wildcard_slots(ins):
            for k in range(s, s + ln):
                if k < len(masked):
                    masked[k] = '??'
        toks.extend(masked)
        n_insns += 1
        if sum(1 for t in toks if t != '??') < 6:
            continue
        pb, pm = compile_pat(toks)
        ref_hits = scan(ref_secs, pb, pm)
        if len(ref_hits) != 1:
            continue
        tgt_hits = scan(tgt_secs, pb, pm)
        return {
            'rva': rva,
            'pattern': ' '.join(toks),
            'bytes': len(toks),
            'insns': n_insns,
            'ref_unique': True,
            'tgt_hits': len(tgt_hits),
            'tgt_rva': tgt_hits[0][0] if len(tgt_hits) == 1 else None,
            'portable': len(tgt_hits) == 1,
        }
    return {'rva': rva, 'error': 'no unique pattern within %d instructions' % max_insns}


if __name__ == '__main__':
    ref = sections(sys.argv[1])
    tgt = sections(sys.argv[2])
    for a in sys.argv[3:]:
        r = extract(ref, tgt, int(a, 16))
        if 'error' in r:
            print('%#010x  FAILED: %s' % (r['rva'], r['error']))
            continue
        verdict = 'PORTABLE' if r['portable'] else ('TARGET_HITS=%d' % r['tgt_hits'])
        print('%#010x  %-14s %2d insns / %2d bytes -> target %s'
              % (r['rva'], verdict, r['insns'], r['bytes'],
                 hex(r['tgt_rva']) if r['tgt_rva'] else '-'))
        print('    %s' % r['pattern'])
