import struct, sys

def rebuild(path, extra_sparse=None):
    d = bytearray(open(path, 'rb').read())
    head_len = struct.unpack('<I', d[0:4])[0]
    cmaps_start = head_len
    clen = struct.unpack('<I', d[cmaps_start:cmaps_start+4])[0]
    count = struct.unpack('<I', d[cmaps_start+8:cmaps_start+12])[0]
    st_off = cmaps_start + 12

    subs = []
    for i in range(count):
        doff, rstart, rlen, gstart, nentries, fmt, pad = struct.unpack(
            '<IIHHHBB', d[st_off+i*16:st_off+(i+1)*16])
        subs.append((doff, rstart, rlen, gstart, nentries, fmt))

    loca_start = cmaps_start + clen
    kern_pos = d.find(b'kern', loca_start)
    if kern_pos == -1:
        kern_pos = len(d)

    slices = []
    for i, (doff, rstart, rlen, gstart, nentries, fmt) in enumerate(subs):
        if nentries == 0:
            slices.append(b'')
            continue
        if nentries == rlen:
            lst = struct.pack('<%dH' % nentries, *range(nentries))
        else:
            abs_off = cmaps_start + doff
            raw = bytes(d[abs_off:abs_off+2*nentries])
            rel = struct.unpack('<%dH' % nentries, raw)
            cps = [rstart + r for r in rel]
            ok = (cps == sorted(set(cps))) and (max(cps) - rstart < 0x10000)
            if not ok and extra_sparse and i in extra_sparse:
                cps = extra_sparse[i]
            lst = struct.pack('<%dH' % len(cps), *[c - rstart for c in cps])
        slices.append(lst)

    data = bytearray()
    data += struct.pack('<I', 0)
    data += b'cmap'
    data += struct.pack('<I', count)
    for i, (doff, rstart, rlen, gstart, nentries, fmt) in enumerate(subs):
        if nentries == 0:
            data += struct.pack('<IIHHHBB', 0, rstart, rlen, gstart, 0, 0, 0)
            continue
        doff_new = 12 + 16*count + sum(len(s) for s in slices[:i])
        data += struct.pack('<IIHHHBB', doff_new, rstart, rlen, gstart, nentries, 2, 0)
    for s in slices:
        data += s
    data[0:4] = struct.pack('<I', len(data))

    out = bytes(d[0:head_len]) + bytes(data) + bytes(d[loca_start:])
    open(path, 'wb').write(out)
    print("%s: rebuilt %d bytes, cmap %d bytes, %d subtables" %
          (path, len(out), len(data), count))

if __name__ == '__main__':
    for p in sys.argv[1:]:
        rebuild(p)
