#!/usr/bin/env python3
"""纯 stdlib PNG 解码:检查左上角水印区域(64,64)-(264,104)是否有黑衬板+白字"""
import struct, zlib, sys

def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not png'
    pos = 8
    width = height = bitdepth = colortype = None
    idat = b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos+4])[0]
        typ = data[pos+4:pos+8]
        chunk = data[pos+8:pos+8+ln]
        if typ == b'IHDR':
            width, height, bitdepth, colortype, _, _, interlace = struct.unpack('>IIBBBBB', chunk)
            assert interlace == 0, 'interlaced not supported'
            assert bitdepth == 8, 'bitdepth %d' % bitdepth
        elif typ == b'IDAT':
            idat += chunk
        elif typ == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = {0:1, 2:3, 4:2, 6:4}[colortype]
    stride = width * ch
    # unfilter
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i-ch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i-ch] if i >= ch else 0
                b = prev[i]
                c = prev[i-ch] if i >= ch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y*stride:(y+1)*stride] = line
        prev = line
    return width, height, ch, bytes(out)

def region_stats(path, x1, y1, x2, y2):
    w, h, ch, pix = read_png(path)
    dark = light = total = 0
    for y in range(y1, min(y2, h)):
        row = y * w * ch
        for x in range(x1, min(x2, w)):
            o = row + x * ch
            r, g, b = pix[o], pix[o+1], pix[o+2]
            lum = (r*30 + g*59 + b*11) // 100
            total += 1
            if lum < 40:
                dark += 1
            elif lum > 220:
                light += 1
    return dark, light, total

if __name__ == '__main__':
    for path in sys.argv[1:]:
        d, l, t = region_stats(path, 64, 64, 264, 104)
        print('%s: dark=%d light=%d total=%d  watermark=%s' %
              (path.split('/')[-1], d, l, t,
               'YES' if (d > 2000 and l > 100) else 'no'))
