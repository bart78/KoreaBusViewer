import struct, sys

def patch_font(path):
    data = bytearray(open(path, 'rb').read())
    head_len = struct.unpack('<I', data[0:4])[0]
    cmaps_start = head_len
    clen = struct.unpack('<I', data[cmaps_start:cmaps_start+4])[0]
    count = struct.unpack('<I', data[cmaps_start+8:cmaps_start+12])[0]
    st_off = cmaps_start + 12

    subs = []
    for i in range(count):
        doff, rstart, rlen, gstart, nentries, fmt, pad = struct.unpack(
            '<IIHHHBB', data[st_off+i*16:st_off+(i+1)*16])
        subs.append((doff, rstart, rlen, gstart, nentries, fmt))

    new_subs = []  # (range_start, range_length, gid_start)
    for doff, rstart, rlen, gstart, nentries, fmt in subs:
        if nentries == 0:
            continue
        if nentries == rlen:
            # dense: fmt0 TINY keeps gid_start + rcp
            new_subs.append((rstart, rlen, gstart))
        else:
            # sparse: read the unicode list (relative codepoints, sorted)
            abs_off = cmaps_start + doff
            rel = struct.unpack('<%dH' % nentries, data[abs_off:abs_off+2*nentries])
            # gid mapping: SPARSE_TINY -> gid_start + index; SPARSE_FULL -> gid_start + ofs_list[i]
            ofs = None
            if fmt == 3:  # SPARSE_FULL: glyph_id_ofs_list follows the unicode list
                ofs = struct.unpack('<%dH' % nentries,
                                    data[abs_off+2*nentries:abs_off+4*nentries])
            for i, r in enumerate(rel):
                cp = rstart + r
                gid = gstart + (ofs[i] if ofs is not None else i)
                new_subs.append((cp, 1, gid))

    # write fmt0 subtables, zero-pad the rest (loader skips len==0)
    for i in range(count):
        data[st_off + i*16 : st_off + (i+1)*16] = b'\x00' * 16
    for i, (rstart, rlen, gstart) in enumerate(new_subs[:count]):
        struct.pack_into('<IIHHHBB', data, st_off + i*16,
                         0, rstart, rlen, gstart, 0, 0, 0)

    open(path, 'wb').write(data)
    print("%s: %d subtables (%d glyph-ranges) patched, %d bytes" %
          (path, count, len(new_subs), len(data)))

if __name__ == '__main__':
    for p in sys.argv[1:]:
        patch_font(p)
