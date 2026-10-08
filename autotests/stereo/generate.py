#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
# SPDX-License-Identifier: CC0-1.0
"""Generate synthetic JPEGs with VRex APP3 and CIPA DC-007 MPF records."""
import pathlib
import struct
import subprocess

root = pathlib.Path(__file__).parent
root.mkdir(exist_ok=True)


def jpeg(color):
    return subprocess.check_output(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                                    f'color={color}:s=16x16', '-frames:v', '1',
                                    '-c:v', 'mjpeg', '-threads', '1', '-f', 'image2pipe', '-'])


def segment(marker, payload):
    return bytes([255, marker]) + struct.pack('>H', len(payload) + 2) + payload


red, blue = jpeg('red'), jpeg('blue')


def jps(descriptor, length=4):
    payload = b'_JPSJPS_' + struct.pack('>HIH', length, descriptor, 0)
    return red[:2] + segment(0xe3, payload) + red[2:]


for layout in (1, 2, 3):
    for left in (0, 1):
        (root / f'test_jps_{layout}_{left}.jpg').write_bytes(jps(1 | layout << 8 | left << 18))
for name, data in {
    'mono.jpg': jps(0),
    'unmarked.jps': red,
    'unmarked.jpg': red,
    'badlength.jps': jps(0x40201, 50),
    'reserved.jps': jps(0x40501),
    'anaglyph_left.jps': jps(0x40401),
    'anaglyph_right.jps': jps(0x401),
    'duplicate.jps': jps(0x40201)[:2] + jps(0x40201)[2:20] + jps(0x40201)[2:],
    'truncated.jps': b'\xff\xd8\xff\xe3\x00\x20_JPSJPS_',
}.items():
    (root / ('test_jps_' + name)).write_bytes(data)


def mpo(order, little, kind=0x020002, bad_offset=False, bad_count=False):
    endian = '<' if little else '>'
    p = lambda fmt, *values: struct.pack(endian + fmt, *values)
    entry = lambda tag, typ, count, value: p('HHII', tag, typ, count, value)
    header = (b'II' if little else b'MM') + p('HI', 42, 8)
    version = int.from_bytes(b'0100', 'little' if little else 'big')
    first_view, second_view = order
    # Three index fields, then two attribute fields, then two 16-byte MP entries.
    attr_offset = 8 + 2 + 3 * 12 + 4
    entries_offset = attr_offset + 2 + 2 * 12 + 4
    index = p('H', 3) + entry(0xb000, 7, 4, version) + entry(0xb001, 4, 1, 3 if bad_count else 2)
    index += entry(0xb002, 7, 32, entries_offset) + p('I', attr_offset)
    attr = p('H', 2) + entry(0xb000, 7, 4, version) + entry(0xb101, 4, 1, first_view) + p('I', 0)
    second_tiff = header + p('H', 2) + entry(0xb000, 7, 4, version) + entry(0xb101, 4, 1, second_view) + p('I', 0)
    second = blue[:2] + segment(0xe2, b'MPF\0' + second_tiff) + blue[2:]
    first_size = len(red) + 4 + 4 + entries_offset + 32
    tiff_base = 2 + 4 + 4
    second_offset = 0xffffffff if bad_offset else first_size - tiff_base
    records = p('IIIHH', kind | 0x20000000, first_size, 0, 0, 0)
    records += p('IIIHH', kind, len(second), second_offset, 0, 0)
    first = red[:2] + segment(0xe2, b'MPF\0' + header + index + attr + records) + red[2:]
    assert len(first) == first_size
    return first + second


for little in (False, True):
    for right in (False, True):
        order = (2, 1) if right else (1, 2)
        (root / f'test_mpo_{int(little)}_{int(right)}.mpo').write_bytes(mpo(order, little))
for name, data in {
    'unknown': mpo((0xffffffff, 1), True),
    'equal': mpo((1, 1), True),
    'composite': mpo((0, 1), True),
    'multangle': mpo((1, 2), True, kind=0x020003),
    'badoffset': mpo((1, 2), True, bad_offset=True),
    'badcount': mpo((1, 2), True, bad_count=True),
    'truncated': mpo((1, 2), True)[:-5],
}.items():
    (root / f'test_mpo_{name}.mpo').write_bytes(data)
