"""Scan the shipped snapmap-plus signature DB against an arbitrary unpacked DOOM image.

Mirrors src/backend/signatures.c: match masked byte patterns over the executable
sections of a PE32+ image, uniqueness required.
"""
import re, struct, sys, json

def strip_comments(s):
    out=[]; i=0; n=len(s)
    while i<n:
        if s.startswith('/*',i):
            j=s.find('*/',i+2); i=(j+2) if j>=0 else n
        elif s.startswith('//',i):
            j=s.find('\n',i); i=(j) if j>=0 else n
        elif s[i]=='"':
            j=i+1
            while j<n and s[j]!='"':
                j+= 2 if s[j]==chr(92) else 1
            out.append(s[i:j+1]); i=j+1
        else:
            out.append(s[i]); i+=1
    return ''.join(out)

def parse_sigs(path):
    src=open(path,encoding='utf-8',errors='replace').read()
    body=src.split('BACKEND_ENGINE_SIGNATURES[] = {',1)[1]
    body=body.split('\n};',1)[0]
    body=strip_comments(body)
    sigs=[]
    for m in re.finditer(r'\{\s*((?:"[^"]*"\s*)+),\s*((?:"[^"]*"\s*)+),\s*(0x[0-9A-Fa-f]+)u?\s*,?\s*\}', body):
        name=''.join(re.findall(r'"([^"]*)"', m.group(1)))
        pat=''.join(re.findall(r'"([^"]*)"', m.group(2)))
        sigs.append((name, pat, int(m.group(3),16)))
    return sigs

def compile_pat(pat):
    toks=pat.split()
    b=bytearray(); mask=bytearray()
    for t in toks:
        if t in ('??','?'): b.append(0); mask.append(0)
        else: b.append(int(t,16)); mask.append(0xFF)
    return bytes(b), bytes(mask)

def exec_sections(path):
    d=open(path,'rb').read()
    e=struct.unpack_from('<I',d,0x3c)[0]
    nsec=struct.unpack_from('<H',d,e+6)[0]
    secoff=e+24+struct.unpack_from('<H',d,e+20)[0]
    secs=[]
    for i in range(nsec):
        o=secoff+i*40
        name=d[o:o+8].rstrip(b'\x00').decode('latin1')
        vs,va,rs,ro=struct.unpack_from('<IIII',d,o+8)
        chars=struct.unpack_from('<I',d,o+36)[0]
        if chars & 0x20000000:  # MEM_EXECUTE
            secs.append((name,va,d[ro:ro+rs]))
    return secs

def find_all(secs, pb, pm):
    hits=[]
    n=len(pb)
    # fast prefilter on first fixed byte run
    fixed=[i for i in range(n) if pm[i]==0xFF]
    if not fixed: return hits
    a0=fixed[0]; b0=pb[a0]
    for name,va,data in secs:
        start=0
        L=len(data)
        while True:
            k=data.find(bytes([b0]), start)
            if k<0: break
            s=k-a0
            start=k+1
            if s<0 or s+n>L: continue
            ok=True
            for i in fixed:
                if data[s+i]!=pb[i]: ok=False; break
            if ok:
                hits.append(va+s)
                if len(hits)>4: return hits
    return hits

if __name__=='__main__':
    sigs=parse_sigs(sys.argv[1])
    results={}
    for img in sys.argv[2:]:
        secs=exec_sections(img)
        rows=[]
        for name,pat,rva in sigs:
            pb,pm=compile_pat(pat)
            h=find_all(secs,pb,pm)
            st='OK' if len(h)==1 else ('NOT_FOUND' if not h else 'AMBIGUOUS')
            rows.append({'name':name,'status':st,'hits':[hex(x) for x in h[:4]],'known_rva':hex(rva)})
        results[img]=rows
    json.dump(results, open('.re-discipline/active/opengl-parity/work/sigscan.json','w'), indent=1)
    for img,rows in results.items():
        ok=sum(1 for r in rows if r['status']=='OK')
        print(f"\n=== {img} : {ok}/{len(rows)} unique ===")
        for r in rows:
            if r['status']!='OK':
                print(f"  {r['status']:10} {r['name']:24} known={r['known_rva']} hits={r['hits']}")
