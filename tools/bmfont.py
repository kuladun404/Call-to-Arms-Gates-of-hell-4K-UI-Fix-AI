"""Чтение/запись бинарного BMFont v3 (.fnt), который использует движок GEM."""
import struct

INFO_FMT = '<hBBHBBBBBBBB'   # fontSize, bitField, charSet, stretchH, aa, padding x4, spacing x2, outline
COMMON_FMT = '<HHHHHBBBBB'   # lineHeight, base, scaleW, scaleH, pages, bitField, alpha, red, green, blue
CHAR_FMT = '<IHHHHhhhBB'     # id, x, y, w, h, xoff, yoff, xadv, page, chnl
KERN_FMT = '<IIh'


def read(data: bytes) -> dict:
    assert data[:4] == b'BMF\x03', data[:4]
    p = 4; f = {'kerning': []}
    while p < len(data):
        t = data[p]; n = struct.unpack_from('<I', data, p + 1)[0]; b = data[p + 5:p + 5 + n]; p += 5 + n
        if t == 1:
            v = struct.unpack_from(INFO_FMT, b); f['info'] = list(v); f['face'] = b[14:].split(b'\0')[0].decode('latin1')
        elif t == 2:
            f['common'] = list(struct.unpack_from(COMMON_FMT, b))
        elif t == 3:
            f['pages'] = [s.decode('latin1') for s in b.split(b'\0') if s]
        elif t == 4:
            sz = struct.calcsize(CHAR_FMT); f['chars'] = [list(struct.unpack_from(CHAR_FMT, b, i)) for i in range(0, n, sz)]
        elif t == 5:
            sz = struct.calcsize(KERN_FMT); f['kerning'] = [list(struct.unpack_from(KERN_FMT, b, i)) for i in range(0, n, sz)]
    return f


def write(f: dict) -> bytes:
    def blk(t, b): return bytes([t]) + struct.pack('<I', len(b)) + b
    out = b'BMF\x03'
    out += blk(1, struct.pack(INFO_FMT, *f['info']) + f['face'].encode('latin1') + b'\0')
    out += blk(2, struct.pack(COMMON_FMT, *f['common']))
    out += blk(3, b''.join(s.encode('latin1') + b'\0' for s in f['pages']))
    out += blk(4, b''.join(struct.pack(CHAR_FMT, *c) for c in f['chars']))
    if f['kerning']:
        out += blk(5, b''.join(struct.pack(KERN_FMT, *k) for k in f['kerning']))
    return out
