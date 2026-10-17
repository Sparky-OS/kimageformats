# SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
# SPDX-License-Identifier: CC0-1.0
"""Reads the items of an ISO/IEC 23008-12 file and writes new files from them (ISO/IEC 14496-12 boxes)."""
import struct


def boxes(data, start=0, end=None):
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack('>I4s', data[pos:pos + 8])
        header = 8
        if size == 1:
            size = struct.unpack('>Q', data[pos + 8:pos + 16])[0]
            header = 16
        elif size == 0:
            size = end - pos
        yield kind.decode('latin1'), pos, pos + header, pos + size
        pos += size


def child(data, start, end, kind):
    for k, at, payload, stop in boxes(data, start, end):
        if k == kind:
            return at, payload, stop
    return None


def read_items(data):
    """{id: {'type', 'data', 'props': [raw property boxes], 'hidden'}}, the primary item's id, and the references."""
    _, meta, meta_end = child(data, 0, len(data), 'meta')
    meta += 4
    items = {}
    _, p, e = child(data, meta, meta_end, 'iinf')
    version = data[p]
    count_size = 2 if version == 0 else 4
    for kind, at, payload, stop in boxes(data, p + 4 + count_size, e):
        v = data[payload]
        flags = int.from_bytes(data[payload + 1:payload + 4], 'big')
        q = payload + 4
        if v >= 2:
            item_id = struct.unpack('>H' if v == 2 else '>I', data[q:q + (2 if v == 2 else 4)])[0]
            q += 2 if v == 2 else 4
            q += 2
            item_type = data[q:q + 4].decode('latin1')
            q += 4
            name_end = data.index(b'\0', q)
            q = name_end + 1
            content_type = ''
            if item_type == 'mime':
                ct_end = data.index(b'\0', q)
                content_type = data[q:ct_end].decode()
            items[item_id] = {'type': item_type, 'props': [], 'hidden': bool(flags & 1), 'content_type': content_type}
    _, p, e = child(data, meta, meta_end, 'pitm')
    primary = struct.unpack('>H' if data[p] == 0 else '>I', data[p + 4:p + (6 if data[p] == 0 else 8)])[0]
    # iloc
    _, p, e = child(data, meta, meta_end, 'iloc')
    version = data[p]
    q = p + 4
    offset_size, length_size = data[q] >> 4, data[q] & 15
    base_size, index_size = data[q + 1] >> 4, (data[q + 1] & 15) if version in (1, 2) else 0
    q += 2
    count = struct.unpack('>H' if version < 2 else '>I', data[q:q + (2 if version < 2 else 4)])[0]
    q += 2 if version < 2 else 4

    def num(size):
        nonlocal q
        value = int.from_bytes(data[q:q + size], 'big') if size else 0
        q += size
        return value
    idat = child(data, meta, meta_end, 'idat')
    for _ in range(count):
        item_id = num(2 if version < 2 else 4)
        method = num(2) & 15 if version in (1, 2) else 0
        num(2)  # data_reference_index
        base = num(base_size)
        chunks = []
        for _ in range(num(2)):
            if index_size:
                num(index_size)
            off = num(offset_size)
            length = num(length_size)
            origin = idat[1] if method == 1 else 0
            chunks.append(data[origin + base + off:origin + base + off + length])
        if item_id in items:
            items[item_id]['data'] = b''.join(chunks)
    # properties
    _, p, e = child(data, meta, meta_end, 'iprp')
    _, cp, ce = child(data, p, e, 'ipco')
    props = [data[at:stop] for kind, at, payload, stop in boxes(data, cp, ce)]
    _, mp, me = child(data, p, e, 'ipma')
    version, flags = data[mp], int.from_bytes(data[mp + 1:mp + 4], 'big')
    q = mp + 4
    for _ in range(struct.unpack('>I', data[q:q + 4])[0] if True else 0):
        break
    count = struct.unpack('>I', data[q:q + 4])[0]
    q += 4
    for _ in range(count):
        item_id = struct.unpack('>H' if version < 1 else '>I', data[q:q + (2 if version < 1 else 4)])[0]
        q += 2 if version < 1 else 4
        n = data[q]
        q += 1
        for _ in range(n):
            if flags & 1:
                index = struct.unpack('>H', data[q:q + 2])[0] & 0x7fff
                q += 2
            else:
                index = data[q] & 0x7f
                q += 1
            if index and item_id in items:
                items[item_id]['props'].append(props[index - 1])
    refs = []
    iref = child(data, meta, meta_end, 'iref')
    if iref:
        v = data[iref[1]]
        for kind, at, payload, stop in boxes(data, iref[1] + 4, iref[2]):
            size = 2 if v == 0 else 4
            src = int.from_bytes(data[payload:payload + size], 'big')
            n = struct.unpack('>H', data[payload + size:payload + size + 2])[0]
            to = [int.from_bytes(data[payload + size + 2 + i * size:payload + size + 2 + (i + 1) * size], 'big') for i in range(n)]
            refs.append((kind, src, to))
    return items, primary, refs


def box(kind, payload):
    return struct.pack('>I4s', 8 + len(payload), kind.encode('latin1')) + payload


def full_box(kind, version, flags, payload):
    return box(kind, struct.pack('>I', version << 24 | flags) + payload)


def write_heif(brand, compatible, items, primary, refs=(), groups=()):
    """items: {id: {'type', 'data', 'props', 'hidden', 'content_type'}}; refs: (type, from, [to]); groups: (type, id, [ids])."""
    ids = sorted(items)
    infe = b''
    for i in ids:
        item = items[i]
        body = struct.pack('>HH4s', i, 0, item['type'].encode()) + b'\0'
        if item['type'] == 'mime':
            body += item['content_type'].encode() + b'\0'
        infe += full_box('infe', 2, 1 if item.get('hidden') else 0, body)
    iinf = full_box('iinf', 0, 0, struct.pack('>H', len(ids)) + infe)
    props, assoc = [], b''
    for i in ids:
        indexes = []
        for prop in items[i]['props']:
            if prop not in props:
                props.append(prop)
            indexes.append(props.index(prop) + 1)
        essential = lambda raw: raw[4:8] in (b'hvcC', b'av1C', b'auxC', b'avcC')
        assoc += struct.pack('>HB', i, len(indexes)) + bytes((0x80 if essential(props[k - 1]) else 0) | k for k in indexes)
    iprp = box('iprp', box('ipco', b''.join(props)) + full_box('ipma', 0, 0, struct.pack('>I', len(ids)) + assoc))
    iref = b''
    if refs:
        iref = full_box('iref', 0, 0, b''.join(box(k, struct.pack('>HH', src, len(to)) + b''.join(struct.pack('>H', t) for t in to)) for k, src, to in refs))
    grpl = b''
    if groups:
        grpl = box('grpl', b''.join(full_box(k, 0, 0, struct.pack('>II', gid, len(members)) + b''.join(struct.pack('>I', m) for m in members))
                                    for k, gid, members in groups))
    hdlr = full_box('hdlr', 0, 0, b'\0\0\0\0pict' + b'\0' * 12 + b'\0')
    pitm = full_box('pitm', 0, 0, struct.pack('>H', primary))
    ftyp = box('ftyp', brand.encode() + b'\0\0\0\0' + b''.join(c.encode() for c in compatible))

    def iloc(base):
        entries = b''
        pos = base
        for i in ids:
            data = items[i]['data']
            entries += struct.pack('>HHHII', i, 0, 1, pos, len(data))
            pos += len(data)
        return full_box('iloc', 0, 0, bytes([0x44, 0x00]) + struct.pack('>H', len(ids)) + entries)
    meta_without_iloc = hdlr + pitm + iinf + iref + iprp + grpl
    meta_size = len(full_box('meta', 0, 0, meta_without_iloc + iloc(0)))
    mdat_start = len(ftyp) + meta_size + 8
    meta = full_box('meta', 0, 0, hdlr + pitm + iloc(mdat_start) + iinf + iref + iprp + grpl)
    mdat = box('mdat', b''.join(items[i]['data'] for i in ids))
    return ftyp + meta + mdat
