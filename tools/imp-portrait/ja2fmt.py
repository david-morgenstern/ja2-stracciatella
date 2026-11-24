"""Minimal readers/writers for Jagged Alliance 2 data formats.

SLF  - the game's resource archives (read only)
STCI - indexed 8-bit images with ETRLE compression (read + write)

Layouts follow rust/stracciatella/src/file_formats/{slf.rs,stci/*}.
ETRLE rows: the game's blitters expect every image row to end with a
0x00 control byte, which is how the original Sir-Tech files are written.
"""
import struct
from dataclasses import dataclass, field


# ---------------------------------------------------------------- SLF

def slf_list(path):
    """Return {lowercase name: (offset, length)} for an SLF archive."""
    with open(path, "rb") as f:
        hdr = f.read(532)
        num_entries = struct.unpack_from("<i", hdr, 512)[0]
        f.seek(-280 * num_entries, 2)
        out = {}
        for _ in range(num_entries):
            e = f.read(280)
            name = e[:256].split(b"\0", 1)[0].decode("ascii").replace("\\", "/").lower()
            off, length, state = struct.unpack_from("<IIB", e, 256)
            if state == 0:
                out[name] = (off, length)
        return out


def slf_read(path, name):
    entries = slf_list(path)
    off, length = entries[name.lower()]
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(length)


# ---------------------------------------------------------------- STCI

@dataclass
class SubImage:
    width: int
    height: int
    pixels: bytes          # width*height palette indices, 0 = transparent
    offset_x: int = 0
    offset_y: int = 0
    app_data: bytes = None  # 16 bytes, or None


@dataclass
class Stci:
    palette: list                       # 256 (r, g, b)
    images: list = field(default_factory=list)
    color_depth: tuple = (8, 8, 8)


def etrle_decode(data, width, height):
    out = bytearray()
    i = 0
    while i < len(data):
        c = data[i]; i += 1
        n = c & 0x7F
        if c & 0x80:
            out += bytes(n)
        elif n:
            out += data[i:i + n]; i += n
        # n == 0 -> end-of-row marker
    if len(out) != width * height:
        raise ValueError(f"ETRLE decoded {len(out)} bytes, expected {width * height}")
    return bytes(out)


def etrle_encode(pixels, width, height):
    out = bytearray()
    for y in range(height):
        row = pixels[y * width:(y + 1) * width]
        x = 0
        while x < width:
            transparent = row[x] == 0
            start = x
            while x < width and (row[x] == 0) == transparent and x - start < 127:
                x += 1
            n = x - start
            if transparent:
                out.append(0x80 | n)
            else:
                out.append(n)
                out += row[start:x]
        out.append(0)  # end of row
    return bytes(out)


def stci_read(data):
    if data[:4] != b"STCI":
        raise ValueError("not an STCI file")
    (orig, stored, transparent, flags, height, width) = struct.unpack_from("<IIIIHH", data, 4)
    if not flags & 0x08:
        raise ValueError("only indexed STCI supported")
    ncolors, nimages = struct.unpack_from("<IH", data, 24)
    depth = tuple(data[30:33])
    app_data_size = struct.unpack_from("<I", data, 48)[0]
    pos = 64
    palette = [tuple(data[pos + 3 * i: pos + 3 * i + 3]) for i in range(ncolors)]
    pos += 3 * ncolors
    headers = []
    for _ in range(nimages):
        headers.append(struct.unpack_from("<IIhhHH", data, pos))
        pos += 16
    base = pos
    images = []
    for (doff, dlen, ox, oy, h, w) in headers:
        px = etrle_decode(data[base + doff: base + doff + dlen], w, h)
        images.append(SubImage(w, h, px, ox, oy))
    if app_data_size:
        pos = base + sum(h[1] for h in headers)
        for img in images:
            img.app_data = data[pos:pos + 16]
            pos += 16
    return Stci(palette, images, depth)


def stci_write(stci):
    assert len(stci.palette) == 256
    blobs = [etrle_encode(im.pixels, im.width, im.height) for im in stci.images]
    has_app = any(im.app_data is not None for im in stci.images)
    app_size = 16 * len(stci.images) if has_app else 0
    original = sum(im.width * im.height for im in stci.images)
    stored = sum(len(b) for b in blobs)
    out = bytearray(b"STCI")
    out += struct.pack("<IIIIHH", original, stored, 0, 0x08 | 0x20, 0, 0)
    out += struct.pack("<IH", 256, len(stci.images)) + bytes(stci.color_depth) + bytes(11)
    out += struct.pack("<B3xI12x", 8, app_size)
    assert len(out) == 64
    for r, g, b in stci.palette:
        out += bytes((r, g, b))
    off = 0
    for im, blob in zip(stci.images, blobs):
        out += struct.pack("<IIhhHH", off, len(blob), im.offset_x, im.offset_y, im.height, im.width)
        off += len(blob)
    for blob in blobs:
        out += blob
    if has_app:
        for im in stci.images:
            out += im.app_data if im.app_data is not None else bytes(16)
    return bytes(out)
