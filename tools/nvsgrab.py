import struct, sys, os
MAGICS = {0x50564331:'PVC1',0x50564332:'PVC2',0x50564333:'PVC3',
          0x50564334:'PVC4',0x50564335:'PVC5',0x50564336:'PVC6',0x50564337:'PVC7'}
def raw_entries(img, off=0x9000, ln=0x3000):
    part = img[off:off+ln]
    for pg in range(0, len(part), 4096):
        page = part[pg:pg+4096]
        if len(page) < 4096: break
        state = int.from_bytes(page[32:64], 'little')
        seq, = struct.unpack('<I', page[4:8])
        i = 0
        while i < 126:
            e = page[64+i*32 : 64+(i+1)*32]
            span = max(e[2], 1)
            key = e[8:24].split(b'\x00')[0].decode('latin1','replace')
            yield dict(page=pg//4096, seq=seq, idx=i, typ=e[1], span=span, chunk=e[3],
                       key=key, data=e[24:32], st=(state>>(2*i))&3,
                       payload=page[64+(i+1)*32 : 64+(i+span)*32])
            i += 1
def grab(img, key='cfg'):
    es = [e for e in raw_entries(img) if e['key']==key]
    datas = [e for e in es if e['typ']==0x42]
    idxs  = [e for e in es if e['typ']==0x48]
    out = []
    for ix in idxs:
        total, = struct.unpack('<I', ix['data'][0:4])
        cnt, cstart = ix['data'][4], ix['data'][5]
        for base in (cstart, 0, 128):
            buf = b''; n = 0; used = []
            while len(buf) < total and n < 16:
                cand = [e for e in datas if e['chunk'] == base + n]
                if not cand: break
                # prefer the entry from the same page generation as the index
                cand.sort(key=lambda e: (abs(e['seq'] - ix['seq']), -e['seq']))
                c = cand[0]
                sz, = struct.unpack('<H', c['data'][0:2])
                buf += c['payload'][:sz]; used.append((c['page'], c['idx'], c['chunk'], sz)); n += 1
            if len(buf) >= 4:
                magic, = struct.unpack('<I', buf[:4])
                if magic in MAGICS and len(buf) >= total:
                    out.append((ix['seq'], total, MAGICS[magic], buf[:total], used))
                    break
    # dedupe, newest first
    seen = set(); res = []
    for r in sorted(out, key=lambda r: -r[0]):
        k = (r[1], r[2])
        if k in seen: continue
        seen.add(k); res.append(r)
    return res
if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit('usage: nvsgrab.py <full-4MB-image.bin>')
    img = open(sys.argv[1],'rb').read()
    for seq,total,mg,blob,used in grab(img):
        print('seq %-5d size %-5d magic %s  chunks %s' % (seq,total,mg,used))
        # Beside the image it came from, not in a world-readable /tmp. This
        # blob is pv_cfg_t: it carries the printer's LAN access code and the
        # hotspot password in plain text, and it was landing at mode 0644
        # inside a 1777 directory every local account can read, and staying.
        d = os.path.dirname(os.path.abspath(sys.argv[1]))
        p = os.path.join(d, '%s.%s.cfg' % (
            os.path.basename(sys.argv[1]).split('-full')[0], mg))
        fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'wb') as f:
            f.write(blob)
        print('   ->', p)
        print('      holds the printer access code and the hotspot password')
        print('      in plain text. Do not commit it or share it.')
