#!/usr/bin/env python3
"""Parse the buslog blobs out of a raw NVS partition dump (esptool read_flash)."""
import struct
import sys

ENTRY_SIZE = 32
ENTRY_COUNT = 126
PAGE_SIZE = 4096

# Item types (nvs.h)
U8 = 0x01
BLOB = 0x42

def parse_pages(data):
    pages = []
    for off in range(0, len(data), PAGE_SIZE):
        page = data[off:off + PAGE_SIZE]
        if len(page) < PAGE_SIZE:
            break
        state = page[0]
        if state == 0xFF:  # uninitialized
            continue
        version = page[1]
        seq_no = struct.unpack('<I', page[4:8])[0]
        # entry table: 2 bits per entry, 32 bytes
        table = page[32:64]
        entries = []
        for i in range(ENTRY_COUNT):
            byte = table[i // 4]
            bits = (byte >> (2 * (i % 4))) & 0x3
            entries.append(bits)
        pages.append({'state': state, 'version': version, 'seq': seq_no, 'table': entries, 'off': off})
    return pages

def parse_items(data, pages):
    items = []
    for pg in pages:
        table = pg['table']
        for i in range(ENTRY_COUNT):
            if table[i] != 0x2:  # not WRITTEN
                continue
            raw = data[pg['off'] + 64 + i * ENTRY_SIZE: pg['off'] + 64 + (i + 1) * ENTRY_SIZE]
            ns = raw[0]
            typ = raw[1]
            span = raw[2]
            key = raw[8:24].split(b'\0')[0].decode('ascii', 'replace')
            items.append({'pg_off': pg['off'], 'idx': i, 'seq': pg['seq'], 'ns': ns, 'type': typ, 'span': span, 'key': key, 'raw': raw})
    return items

def blob_value(data, item):
    raw = item['raw']
    data_size = struct.unpack('<H', raw[24:26])[0]
    data_crc = struct.unpack('<I', raw[28:32])[0]
    # blob data lives in the entry slots following the metadata entry
    blob = b''
    i = item['idx'] + 1
    off = item['pg_off'] + 64 + i * ENTRY_SIZE
    blob = data[off:off + data_size]
    return blob, data_crc

def load_nvs(path):
    data = open(path, 'rb').read()
    pages = parse_pages(data)
    items = parse_items(data, pages)

    # namespace map: u8 items in ns 0
    ns_map = {}
    for it in items:
        if it['ns'] == 0 and it['type'] == U8:
            ns_map[it['key']] = it['raw'][24]

    # keep latest per (ns, key)
    latest = {}
    for it in items:
        if it['ns'] == 0:
            continue
        if it['type'] == BLOB and it['span'] > 1:
            v = (it['seq'], it['pg_off'], it['idx'])
            key = (it['ns'], it['key'])
            if key not in latest or latest[key][0] < v[0]:
                latest[key] = v

    out = {}
    for (ns, key), (seq, pg_off, idx) in latest.items():
        full = None
        for it in items:
            if it['pg_off'] == pg_off and it['idx'] == idx:
                full = it
                break
        blob, _ = blob_value(data, full)
        out[(ns, key)] = blob
    return ns_map, out

def decode_events(blob):
    """2-byte events: type(1)<<14 | route_idx(3)<<11 | minute(11)"""
    evs = []
    for i in range(0, len(blob) - 1, 2):
        ev = blob[i] | (blob[i + 1] << 8)
        typ = ev >> 14
        route = (ev >> 11) & 0x7
        minute = ev & 0x7FF
        evs.append((typ, route, minute))
    return evs

ROUTES = [32, 73, 310, 340, 4103, 9409, 9507]

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'nvs.bin'
    ns_map, blobs = load_nvs(path)
    print('namespaces:', ns_map)
    for (ns, key), blob in sorted(blobs.items()):
        print(f'ns={ns} key={key} bytes={len(blob)} events={len(blob)//2}')
