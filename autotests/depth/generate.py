#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
# SPDX-License-Identifier: CC0-1.0
"""Generate pictures with depth maps in every container the readers know, small enough for the tests:

HEIF and AVIF (ISO/IEC 23008-12): a depth auxiliary image (auxC urn:mpeg:hevc:2015:auxid:2, urn:mpeg:avc:2015:auxid:2,
urn:mpeg:mpegB:cicp:systems:auxiliary:depth) with a depth representation information SEI message in its aux_subtype
(ITU-T H.265 G.14.2.4 and H.264 Annex H, types 0 to 3), Apple's depth description in the depth image's XMP, stereo pair
entity groups ('ster', ISO/IEC 23008-12:2025 6.8.5), and the auxiliary images that carry no depth (a portrait matte,
a gain map).
JPEG: Dynamic Depth 1.0 (a Container of concatenated items, attribute and element XMP), GDepth (main and extended XMP,
XMP Specification Part 3 1.1.3.1), Apple's Multi-Picture Format images (CIPA DC-007) with their depth descriptions.

    generate.py [OUTDIR] [WIDTH HEIGHT]

Needs heif-enc (libheif, with the x265 and aom encoders) and Python's PIL.
"""
import io
import math
import pathlib
import struct
import subprocess
import sys
import tempfile

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import isobmff  # noqa: E402

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).parent
W, H = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (64, 48)
root.mkdir(parents=True, exist_ok=True)

# The depth regions, as nearness 0 to 1: a far background, a box at mid depth, a near disc.
LEVELS = {'background': 0.25, 'box': 0.63, 'disc': 0.9}


def region(x, y, w, h):
    if (x - 0.7 * w) ** 2 + (y - 0.5 * h) ** 2 < (0.18 * h) ** 2:
        return 'disc'
    if 0.12 * w <= x < 0.38 * w and 0.17 * h <= y < 0.83 * h:
        return 'box'
    return 'background'


def picture(w=W, h=H):
    image = Image.new('RGB', (w, h))
    px = image.load()
    for y in range(h):
        for x in range(w):
            # detail everywhere, so each eye's position can be found by block matching
            v = (x * 37 + y * 91 + ((x * y) % 23) * 11) % 256
            px[x, y] = (v, (255 - v + 64 * (x % 3)) % 256, (x * 7 + y * 13) % 256)
    return image


def depth(w, h, bits=8, invert=False):
    # more than 8 bits: a 16-bit image, which heif-enc -b reduces to its most significant bits
    maxval = 65535 if bits > 8 else 255
    image = Image.new('I;16' if bits > 8 else 'L', (w, h))
    px = image.load()
    for y in range(h):
        for x in range(w):
            level = LEVELS[region(x * W / w, y * H / h, W, H)]
            px[x, y] = round((1 - level if invert else level) * maxval)
    return image


def encode(image, codec, lossless=False, bits=None):
    """The primary item of a heif-enc encoding: type, data and properties."""
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / 'in.png'
        out = pathlib.Path(tmp) / ('out.avif' if codec == 'av1' else 'out.heic')
        image.save(src)
        args = ['heif-enc', '-o', str(out)] + (['-A'] if codec == 'av1' else []) + (['-L'] if lossless else ['-q', '90'])
        if bits:
            args += ['-b', str(bits)]
        subprocess.run(args + [str(src)], check=True, stdout=subprocess.DEVNULL)
        items, primary, _ = isobmff.read_items(out.read_bytes())
        item = items[primary]
        return {'type': item['type'], 'data': item['data'], 'props': [p for p in item['props'] if p[4:8] in (b'hvcC', b'av1C', b'ispe', b'pixi', b'colr', b'clap')]}


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, n, value):
        self.bits += [(value >> (n - 1 - i)) & 1 for i in range(n)]

    def ue(self, value):
        value += 1
        n = value.bit_length()
        self.u(n - 1, 0)
        self.u(n, value)

    def element(self, x):
        # depth_rep_info_element(): x = (-1)^s * 2^(e - 31) * (1 + n / 2^v), with a 32-bit mantissa
        sign = 1 if x < 0 else 0
        if x == 0:
            self.u(1, 0), self.u(7, 0), self.u(5, 0), self.u(1, 0)
            return
        m, ex = math.frexp(abs(x))
        e = ex + 30
        n = round((2 * m - 1) * (1 << 32))
        if n == 1 << 32:
            n, e = 0, e + 1
        self.u(1, sign), self.u(7, e), self.u(5, 31), self.u(32, n)

    def bytes(self, trailing):
        bits = list(self.bits)
        if trailing or len(bits) % 8:
            bits.append(1)
        while len(bits) % 8:
            bits.append(0)
        return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def depth_representation(kind, avc=False, z_near=None, z_far=None, d_min=None, d_max=None, model=None):
    b = Bits()
    if avc:
        b.u(1, 1)  # all_views_equal_flag
    b.u(1, z_near is not None), b.u(1, z_far is not None)
    if avc and (z_near is not None or z_far is not None):
        b.u(1, 1), b.ue(0)  # z_axis_equal_flag, common_z_axis_reference_view
    b.u(1, d_min is not None), b.u(1, d_max is not None)
    b.ue(kind)
    if avc:
        b.ue(0)  # depth_info_view_id[0]
        if d_min is not None or d_max is not None:
            b.ue(0)  # disparity_reference_view[0]
    elif d_min is not None or d_max is not None:
        b.ue(0)  # disparity_ref_view_id
    for value in (z_near, z_far, d_min, d_max):
        if value is not None:
            b.element(value)
    if kind == 3:
        b.ue(len(model) - 1)
        for value in model:
            b.ue(value)
    return b.bytes(False)


def escape(rbsp):
    out, zeros = bytearray(), 0
    for byte in rbsp:
        if zeros >= 2 and byte <= 3:
            out.append(3)
            zeros = 0
        out.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(out)


def sei_subtype(payload, avc=False):
    """An aux_subtype as libheif and Apple write it: a 32-bit length, then the SEI NAL unit with a 32-bit length."""
    message = bytes([50 if avc else 177, len(payload)]) + payload + b'\x80'
    nal = (b'\x06' if avc else b'\x4e\x01') + escape(message)
    return struct.pack('>II', 4 + len(nal), len(nal)) + nal


def aux_c(urn, subtype=b''):
    return isobmff.full_box('auxC', 0, 0, urn.encode() + b'\0' + subtype)


def xmp_item(text):
    return {'type': 'mime', 'content_type': 'application/rdf+xml', 'data': text.encode(), 'props': []}


def heif(name, primary, extra=None, refs=(), groups=(), brand='heic'):
    items = {1: primary}
    items.update(extra or {})
    compatible = ['mif1', 'miaf', brand] if brand == 'avif' else ['mif1', 'heic', 'miaf']
    (root / name).write_bytes(isobmff.write_heif(brand, compatible, items, 1, refs, groups))


def with_depth(name, codec, subtype, urn='urn:mpeg:hevc:2015:auxid:2', xmp=None, bits=8, size=None):
    dw, dh = size or (W, H)
    d = encode(depth(dw, dh, bits), codec, lossless=True, bits=bits if bits > 8 else None)
    d['props'] = d['props'] + [aux_c(urn, subtype)]
    d['hidden'] = True
    extra = {2: d}
    refs = [('auxl', 2, [1])]
    if xmp:
        extra[3] = xmp_item(xmp)
        refs.append(('cdsc', 3, [2]))
    heif(name, pic_hevc if codec == 'hevc' else pic_av1, extra, refs, brand='avif' if codec == 'av1' else 'heic')


APPLE_XMP = '''<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="XMP Core 6.0.0">
   <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
      <rdf:Description rdf:about=""
            xmlns:apdi="http://ns.apple.com/pixeldatainfo/1.0/"
            xmlns:depthData="http://ns.apple.com/depthData/1.0/">
         <apdi:IntMaxValue>255</apdi:IntMaxValue>
         <apdi:StoredFormat>1278226488</apdi:StoredFormat>
         <apdi:NativeFormat>{native}</apdi:NativeFormat>
         <apdi:IntMinValue>0</apdi:IntMinValue>
         <apdi:FloatMaxValue>{fmax}</apdi:FloatMaxValue>
         <apdi:FloatMinValue>{fmin}</apdi:FloatMinValue>
         <apdi:AuxiliaryImageType>{aux}</apdi:AuxiliaryImageType>
         <depthData:Accuracy>relative</depthData:Accuracy>
         <depthData:Filtered>True</depthData:Filtered>
      </rdf:Description>
   </rdf:RDF>
</x:xmpmeta>'''
HDIS, HDEP = 0x68646973, 0x68646570  # 'hdis', 'hdep'

pic = picture()
pic_hevc = encode(pic, 'hevc')
pic_av1 = encode(pic, 'av1')

# ---- HEIF and AVIF
heif('heif_mono.heic', pic_hevc)
heif('avif_mono.avif', pic_av1, brand='avif')
with_depth('heif_depth_plain.heic', 'hevc', b'')
with_depth('heif_depth_inverse_z.heic', 'hevc', sei_subtype(depth_representation(0, z_near=0.5, z_far=8)))
with_depth('heif_depth_disparity.heic', 'hevc', sei_subtype(depth_representation(1, d_min=-6.5, d_max=19.25)))
with_depth('heif_depth_z.heic', 'hevc', sei_subtype(depth_representation(2, z_near=1.25, z_far=40)), size=(W // 2, H // 2))
with_depth('heif_depth_nonlinear.heic', 'hevc', sei_subtype(depth_representation(3, d_min=-4, d_max=12, model=[20, 7])))
with_depth('heif_depth_avc_sei.heic', 'hevc', sei_subtype(depth_representation(0, avc=True, z_near=2, z_far=30), avc=True),
           urn='urn:mpeg:avc:2015:auxid:2')
with_depth('heif_depth_10bit.heic', 'hevc', sei_subtype(depth_representation(0, z_near=0.75, z_far=12)), bits=10)
with_depth('heif_depth_apple.heic', 'hevc', sei_subtype(depth_representation(1, d_min=1.5, d_max=4.5)),
           xmp=APPLE_XMP.format(native=HDIS, fmax=4.5, fmin=1.5, aux='disparity'))
with_depth('heif_depth_apple_metres.heic', 'hevc', b'', xmp=APPLE_XMP.format(native=HDEP, fmax=3.5, fmin=0.4, aux='depth'))
with_depth('heif_matte_apple.heic', 'hevc', b'', urn='urn:com:apple:photo:2018:aux:portraiteffectsmatte')
with_depth('heif_gainmap_apple.heic', 'hevc', b'', urn='urn:com:apple:photo:2020:aux:hdrgainmap')
with_depth('avif_depth.avif', 'av1', sei_subtype(depth_representation(0, z_near=1, z_far=20)), urn='urn:mpeg:mpegB:cicp:systems:auxiliary:depth')
with_depth('avif_depth_plain.avif', 'av1', b'', urn='urn:mpeg:mpegB:cicp:systems:auxiliary:depth')

right = encode(Image.eval(pic, lambda v: 255 - v), 'hevc')
heif('heif_stereo_left_primary.heic', pic_hevc, {2: right}, groups=[('ster', 10, [1, 2])])
heif('heif_stereo_right_primary.heic', pic_hevc, {2: right}, groups=[('ster', 10, [2, 1])])

# ---- JPEG


def jpeg(image, quality=95, exif=None):
    out = io.BytesIO()
    image.save(out, 'JPEG', quality=quality, **({'exif': exif} if exif else {}))
    return out.getvalue()


def png(image):
    out = io.BytesIO()
    image.save(out, 'PNG')
    return out.getvalue()


def segment(marker, payload):
    return bytes([255, marker]) + struct.pack('>H', len(payload) + 2) + payload


def insert(jpeg_bytes, segments):
    """The segments right after SOI and any APP0/APP1 segments the encoder wrote."""
    pos = 2
    while jpeg_bytes[pos] == 0xff and jpeg_bytes[pos + 1] in (0xe0, 0xe1):
        pos += 2 + struct.unpack('>H', jpeg_bytes[pos + 2:pos + 4])[0]
    return jpeg_bytes[:pos] + b''.join(segments) + jpeg_bytes[pos:]


def xmp_packet(body, namespaces):
    ns = ''.join(f' xmlns:{k}="{v}"' for k, v in namespaces.items())
    return (f'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
            f'<rdf:Description rdf:about=""{ns}>{body}</rdf:Description></rdf:RDF></x:xmpmeta>').encode()


def xmp_segments(main, extended=None, chunk=60000):
    """APP1 segments for a main packet and its extension, cut in chunks (XMP Specification Part 3, 1.1.3.1)."""
    import hashlib
    segments = [segment(0xe1, b'http://ns.adobe.com/xap/1.0/\0' + main)]
    if extended:
        guid = hashlib.md5(extended).hexdigest().upper().encode()
        for offset in range(0, len(extended), chunk):
            segments.append(segment(0xe1, b'http://ns.adobe.com/xmp/extension/\0' + guid + struct.pack('>II', len(extended), offset)
                                    + extended[offset:offset + chunk]))
        return segments, guid.decode()
    return segments, None


NS = {
    'Device': 'http://ns.google.com/photos/dd/1.0/device/',
    'Container': 'http://ns.google.com/photos/dd/1.0/container/',
    'Item': 'http://ns.google.com/photos/dd/1.0/item/',
    'Camera': 'http://ns.google.com/photos/dd/1.0/camera/',
    'DepthMap': 'http://ns.google.com/photos/dd/1.0/depthmap/',
    'Image': 'http://ns.google.com/photos/dd/1.0/image/',
    'Profile': 'http://ns.google.com/photos/dd/1.0/profile/',
}


def dynamic_depth(name, depth_bytes, depth_mime, fmt, near, far, original=None, semantic='Depth', elements=False, exif=None,
                  padding=0, trailing=b''):
    primary = jpeg(pic, exif=exif)
    items = [('image/jpeg', 0, 'primary_image')]
    tail = [depth_bytes]
    items.append((depth_mime, len(depth_bytes), 'android/depthmap'))
    confidence = jpeg(Image.new('L', (W // 2, H // 2), 255))
    items.append(('image/jpeg', len(confidence), 'android/confidencemap'))
    tail.append(confidence)
    if original:
        items.append(('image/jpeg', len(original), 'android/original'))
        tail.append(original)
    if elements:
        # the element form of the same properties
        item = lambda mime, length, uri: (f'<rdf:li rdf:parseType="Resource"><Container:Item rdf:parseType="Resource"><Item:Mime>{mime}</Item:Mime>'
                                          f'<Item:Length>{length}</Item:Length><Item:DataURI>{uri}</Item:DataURI>'
                                          + (f'<Item:Padding>{padding}</Item:Padding>' if uri == 'primary_image' and padding else '')
                                          + '</Container:Item></rdf:li>')
        dmap = (f'<Camera:DepthMap rdf:parseType="Resource"><DepthMap:ItemSemantic>{semantic}</DepthMap:ItemSemantic>'
                f'<DepthMap:Format>{fmt}</DepthMap:Format><DepthMap:Units>Meters</DepthMap:Units><DepthMap:Near>{near}</DepthMap:Near>'
                f'<DepthMap:Far>{far}</DepthMap:Far><DepthMap:DepthURI>android/depthmap</DepthMap:DepthURI></Camera:DepthMap>')
        image = ('<Camera:Image rdf:parseType="Resource"><Image:ItemSemantic>Original</Image:ItemSemantic>'
                 '<Image:ItemURI>android/original</Image:ItemURI></Camera:Image>') if original else ''
    else:
        item = lambda mime, length, uri: (f'<rdf:li rdf:parseType="Resource"><Container:Item Item:Mime="{mime}" Item:Length="{length}" Item:DataURI="{uri}"'
                                          + (f' Item:Padding="{padding}"' if uri == 'primary_image' and padding else '') + '/></rdf:li>')
        dmap = (f'<Camera:DepthMap DepthMap:ItemSemantic="{semantic}" DepthMap:Format="{fmt}" DepthMap:Units="Meters" DepthMap:Near="{near}"'
                f' DepthMap:Far="{far}" DepthMap:DepthURI="android/depthmap" DepthMap:ConfidenceURI="android/confidencemap"'
                f' DepthMap:MeasureType="OpticalAxis"/>')
        image = '<Camera:Image Image:ItemSemantic="Original" Image:ItemURI="android/original"/>' if original else ''
    device = (f'<Device:Profiles><rdf:Seq><rdf:li rdf:parseType="Resource"><Device:Profile Profile:Type="DepthPhoto">'
              f'<Profile:CameraIndices><rdf:Seq><rdf:li>0</rdf:li></rdf:Seq></Profile:CameraIndices></Device:Profile></rdf:li></rdf:Seq></Device:Profiles>'
              f'<Device:Cameras><rdf:Seq><rdf:li rdf:parseType="Resource"><Device:Camera Camera:Trait="Physical">{dmap}{image}</Device:Camera></rdf:li>'
              f'</rdf:Seq></Device:Cameras>'
              f'<Device:Container><Container:Directory><rdf:Seq>{"".join(item(*i) for i in items)}</rdf:Seq></Container:Directory></Device:Container>')
    # Android's writer puts the Device in the extended section (dynamic_depth Device::SerializeToXmp)
    extended = xmp_packet(device, NS)
    segments, guid = xmp_segments(b'', extended)
    main = xmp_packet('', {'xmpNote': 'http://ns.adobe.com/xmp/note/'}).replace(b'rdf:about=""', f'rdf:about="" xmpNote:HasExtendedXMP="{guid}"'.encode())
    segments, _ = xmp_segments(main, extended)
    (root / name).write_bytes(insert(primary, segments) + b'\0' * padding + b''.join(tail) + trailing)


def exif_orientation(value):
    exif = Image.Exif()
    exif[0x0112] = value
    return exif.tobytes()


def rangeinverse(n, f, bits=8):
    """Depth map samples by Dynamic Depth's RangeInverse: (far * (d - near)) / (d * (far - near)), d from the regions' nearness."""
    maxval = (1 << bits) - 1
    image = Image.new('I;16' if bits > 8 else 'L', (W // 2, H // 2))
    px = image.load()
    for y in range(H // 2):
        for x in range(W // 2):
            level = LEVELS[region(x * 2, y * 2, W, H)]
            d = 1 / (1 / f + level * (1 / n - 1 / f))  # the nearest level at 1/near
            px[x, y] = math.floor(f * (d - n) / (d * (f - n)) * maxval)
    return image


dd8 = jpeg(rangeinverse(0.3, 6), quality=100)
dynamic_depth('jpeg_dynamic_depth.jpg', dd8, 'image/jpeg', 'RangeInverse', 0.3, 6, original=jpeg(Image.eval(pic, lambda v: v // 2 + 64)))
dynamic_depth('jpeg_dynamic_depth_png16.jpg', png(depth(W // 2, H // 2, 16, invert=True)), 'image/png', 'RangeLinear', 0.5, 9, elements=True)
dynamic_depth('jpeg_dynamic_depth_rotated.jpg', jpeg(rangeinverse(0.3, 6), quality=100, exif=exif_orientation(6)), 'image/jpeg', 'RangeInverse', 0.3, 6,
              exif=exif_orientation(6))
dynamic_depth('jpeg_dynamic_depth_padding.jpg', dd8, 'image/jpeg', 'RangeInverse', 0.3, 6, padding=16, trailing=b'')
dynamic_depth('jpeg_dynamic_depth_segmentation.jpg', dd8, 'image/jpeg', 'RangeInverse', 0.3, 6, semantic='Segmentation')


def gdepth(name, fmt, near, far, depth_bytes, mime, original=None):
    import base64
    main = xmp_packet('', {'GDepth': 'http://ns.google.com/photos/1.0/depthmap/', 'xmpNote': 'http://ns.adobe.com/xmp/note/'})
    body = f'<GDepth:Data>{base64.b64encode(depth_bytes).decode()}</GDepth:Data>'
    if original:
        body += f'<GImage:Mime>image/jpeg</GImage:Mime><GImage:Data>{base64.b64encode(original).decode()}</GImage:Data>'
    extended = xmp_packet(body, {'GDepth': 'http://ns.google.com/photos/1.0/depthmap/', 'GImage': 'http://ns.google.com/photos/1.0/image/'})
    _, guid = xmp_segments(b'', extended)
    main = main.replace(b'rdf:about=""', (f'rdf:about="" GDepth:Format="{fmt}" GDepth:Near="{near}" GDepth:Far="{far}" GDepth:Mime="{mime}"'
                                          f' GDepth:Units="m" xmpNote:HasExtendedXMP="{guid}"').encode())
    segments, _ = xmp_segments(main, extended, chunk=1000)
    (root / name).write_bytes(insert(jpeg(pic), segments))


gdepth('jpeg_gdepth_inverse.jpg', 'RangeInverse', 0.4, 5, png(rangeinverse(0.4, 5)), 'image/png', original=jpeg(Image.eval(pic, lambda v: v // 2)))
gdepth('jpeg_gdepth_linear.jpg', 'RangeLinear', 1, 3, jpeg(depth(W // 2, H // 2, invert=True), quality=100), 'image/jpeg')


def mpf(images, types):
    """A primary JPEG whose CIPA DC-007 MP Index lists the other images, appended after it."""
    def index(offsets, sizes):
        tiff = b'MM\0*' + struct.pack('>I', 8)
        count = len(images)
        entries_offset = 8 + 2 + 3 * 12 + 4
        tiff += struct.pack('>H', 3) + struct.pack('>HHI4s', 0xb000, 7, 4, b'0100') + struct.pack('>HHII', 0xb001, 4, 1, count)
        tiff += struct.pack('>HHII', 0xb002, 7, 16 * count, entries_offset) + struct.pack('>I', 0)
        for i in range(count):
            tiff += struct.pack('>IIIHH', types[i], sizes[i], offsets[i], 0, 0)
        return segment(0xe2, b'MPF\0' + tiff)
    placeholder = insert(images[0], [index([0] * len(images), [0] * len(images))])
    tiff_start = placeholder.index(b'MPF\0') + 4
    sizes = [len(placeholder)] + [len(i) for i in images[1:]]
    offsets, pos = [0], len(placeholder)
    for i in images[1:]:
        offsets.append(pos - tiff_start)
        pos += len(i)
    return insert(images[0], [index(offsets, sizes)]) + b''.join(images[1:])


def apple_aux(image, xmp):
    return insert(jpeg(image, quality=100), [segment(0xe1, b'http://ns.adobe.com/xap/1.0/\0' + xmp.encode())])


GAIN = '''<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description rdf:about=""
 xmlns:apdi="http://ns.apple.com/pixeldatainfo/1.0/" xmlns:HDRGainMap="http://ns.apple.com/HDRGainMap/1.0/"><apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</apdi:AuxiliaryImageType>
 <apdi:StoredFormat>1278226488</apdi:StoredFormat><apdi:NativeFormat>1278226488</apdi:NativeFormat><HDRGainMap:HDRGainMapVersion>65536</HDRGainMap:HDRGainMapVersion>
 </rdf:Description></rdf:RDF></x:xmpmeta>'''
MATTE = GAIN.replace('urn:com:apple:photo:2020:aux:hdrgainmap', 'urn:com:apple:photo:2018:aux:portraiteffectsmatte')
gain = apple_aux(Image.new('L', (W // 2, H // 2), 128), GAIN)
(root / 'jpeg_apple_depth.jpg').write_bytes(mpf([jpeg(pic), gain, apple_aux(depth(W // 2, H // 2),
                                                                              APPLE_XMP.format(native=HDIS, fmax=4.8, fmin=0.25, aux='disparity'))],
                                                [0x030000, 0, 0]))
(root / 'jpeg_apple_matte.jpg').write_bytes(mpf([jpeg(pic), apple_aux(Image.new('L', (W, H), 200), MATTE)], [0x030000, 0]))
(root / 'jpeg_gainmap.jpg').write_bytes(mpf([jpeg(pic), gain], [0x030000, 0]))
(root / 'jpeg_mono.jpg').write_bytes(jpeg(pic))
