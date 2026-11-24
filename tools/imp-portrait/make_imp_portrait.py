"""Turn a photo into the set of face images an I.M.P. portrait needs.

    python make_imp_portrait.py photo.jpg --face 216 --out <mod>/data/faces \
        --big-box 418 165 838 648 --small-box 440 200 820 540 \
        --eyes 642 372 --mouth 668 460

Boxes and points are in photo pixel coordinates:
  --big-box    crop for FACES/bigfaces/<n>.sti (106x122, shown on the IMP and AIM pages)
  --small-box  crop for FACES/<n>.sti (48x43 talking head), 65face (31x27) and 33face (15x14)
  --eyes       centre between the eyes       (blink frames are cut around it)
  --mouth      centre of the mouth           (talking frames are drawn around it)

Prints the "eyes" and "mouth" offsets to put into imp.json.
Writes PNG previews next to the STIs when --preview is given.
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageEnhance

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ja2fmt  # noqa: E402

# frame layout of the original talking-head files (FACES/200.sti etc.)
TALKING_SIZES = {
    "":        {"face": (48, 43), "eyes": (31, 14), "mouth": (32, 22)},
    "65face/": {"face": (31, 27), "eyes": (20, 9),  "mouth": (20, 14)},
    "33face/": {"face": (15, 14), "eyes": (10, 4),  "mouth": (10, 7)},
}
BIG_SIZE = (106, 122)


def mouth_frames(photo, mouth, open_steps=(0.0, 0.5, 1.0)):
    """Return the photo with the mouth drawn progressively open, one per step."""
    mx, my = mouth
    frames = []
    for step in open_steps:
        im = photo.copy()
        if step > 0:
            d = ImageDraw.Draw(im)
            w = 46
            h = 8 + 16 * step
            d.ellipse([mx - w / 2, my - h / 2, mx + w / 2, my + h / 2], fill=(52, 22, 18))
            if step >= 0.5:  # a hint of upper teeth
                d.ellipse([mx - w / 2 + 6, my - h / 2 + 1, mx + w / 2 - 6, my - h / 2 + 5], fill=(190, 178, 160))
        frames.append(im)
    return frames


def lid_frames(photo, eyes, closed_steps=(1.0, 0.5, 0.0, 0.0)):
    """Return the photo with eyelids drawn over both eyes, one per blink step
    (1.0 = shut). The lid takes the skin colour just above each eye."""
    frames = []
    for step in closed_steps:
        im = photo.copy()
        if step > 0:
            d = ImageDraw.Draw(im)
            for ex, ey in eyes:
                w, h = 78, 34
                # lids sit in the eye's shadow: a bit darker than the skin above
                skin = tuple(int(c * 0.82) for c in im.getpixel((int(ex), int(ey - h * 1.1))))
                d.ellipse([ex - w / 2, ey - h / 2, ex + w / 2, ey - h / 2 + h * step], fill=skin)
                d.line([ex - w / 2 + 6, ey - h / 2 + h * step, ex + w / 2 - 6, ey - h / 2 + h * step],
                       fill=tuple(max(0, c - 25) for c in skin), width=2)
        frames.append(im)
    return frames


def crop_scale(photo, box, size):
    return photo.crop(box).resize(size, Image.LANCZOS)


def to_px(point, box, size):
    x0, y0, x1, y1 = box
    return ((point[0] - x0) * size[0] / (x1 - x0), (point[1] - y0) * size[1] / (y1 - y0))


def quantize(images):
    """Quantize RGB images to one shared 255-colour palette, index 0 kept free (transparent)."""
    total_w = sum(i.width for i in images)
    h = max(i.height for i in images)
    sheet = Image.new("RGB", (total_w, h))
    x = 0
    for i in images:
        sheet.paste(i, (x, 0))
        x += i.width
    q = sheet.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[: 255 * 3]
    pal += [0] * (255 * 3 - len(pal))  # a small image may need fewer colours
    palette = [(0, 0, 0)] + [tuple(pal[3 * k: 3 * k + 3]) for k in range(255)]
    out = []
    for im in images:
        qi = im.quantize(palette=q, dither=Image.Dither.NONE)
        out.append(bytes(b + 1 for b in qi.tobytes()))
    return palette, out


def write_sti(path, frames, preview):
    palette, pixels = quantize(frames)
    stci = ja2fmt.Stci(palette, [ja2fmt.SubImage(f.width, f.height, px) for f, px in zip(frames, pixels)])
    data = ja2fmt.stci_write(stci)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)
    # verify it reads back
    back = ja2fmt.stci_read(data)
    assert [(i.width, i.height) for i in back.images] == [(f.width, f.height) for f in frames]
    if preview:
        sheet = Image.new("RGB", (sum(f.width + 4 for f in frames), max(f.height for f in frames)), (255, 0, 255))
        x = 0
        for img in back.images:
            p = Image.new("P", (img.width, img.height))
            p.putdata(list(img.pixels))
            p.putpalette([c for rgb in back.palette for c in rgb])
            sheet.paste(p.convert("RGB"), (x, 0))
            x += img.width + 4
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(path[:-4] + ".preview.png")
    print(f"wrote {path} ({len(data)} bytes, {len(frames)} frames)")


def clamp_box(cx, cy, w, h, W, H):
    x = int(round(cx - w / 2))
    y = int(round(cy - h / 2))
    return max(0, min(W - w, x)), max(0, min(H - h, y))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("photo")
    ap.add_argument("--face", type=int, required=True)
    ap.add_argument("--out", required=True, help="the faces directory, e.g. <mod>/data/faces")
    ap.add_argument("--big-box", type=int, nargs=4, required=True)
    ap.add_argument("--small-box", type=int, nargs=4, required=True)
    ap.add_argument("--eyes", type=float, nargs=2, required=True)
    ap.add_argument("--mouth", type=float, nargs=2, required=True)
    ap.add_argument("--eyelids", type=float, nargs=4, metavar=("LX", "LY", "RX", "RY"),
                    help="left and right eye centres: draw real blinks instead of darkening (for open eyes)")
    ap.add_argument("--darken", type=float, default=0.85, help="brightness factor; JA2 faces are moody")
    ap.add_argument("--preview", action="store_true")
    a = ap.parse_args()

    photo = Image.open(a.photo).convert("RGB")
    photo = ImageEnhance.Brightness(photo).enhance(a.darken)
    photo = ImageEnhance.Contrast(photo).enhance(1.1)

    # big portrait: a single frame
    write_sti(os.path.join(a.out, "bigfaces", f"{a.face}.sti"), [crop_scale(photo, a.big_box, BIG_SIZE)], a.preview)

    mouths = mouth_frames(photo, a.mouth)
    lids = lid_frames(photo, [(a.eyelids[0], a.eyelids[1]), (a.eyelids[2], a.eyelids[3])]) if a.eyelids else None
    offsets = None
    for sub, sz in TALKING_SIZES.items():
        face = crop_scale(photo, a.small_box, sz["face"])
        frames_full = [crop_scale(m, a.small_box, sz["face"]) for m in mouths]
        ex, ey = to_px(a.eyes, a.small_box, sz["face"])
        mx, my = to_px(a.mouth, a.small_box, sz["face"])
        eW, eH = sz["eyes"]
        mW, mH = sz["mouth"]
        exo, eyo = clamp_box(ex, ey, eW, eH, *sz["face"])
        mxo, myo = clamp_box(mx, my, mW, mH, *sz["face"])
        frames = [face]
        # 4 eye frames: the sunglasses (or open eyes) region, blink frames slightly darkened
        for k in range(4):
            if lids:
                eye = crop_scale(lids[k], a.small_box, sz["face"]).crop((exo, eyo, exo + eW, eyo + eH))
            else:
                eye = face.crop((exo, eyo, exo + eW, eyo + eH))
                if k in (1, 2):  # half/fully closed lids read as a slight darkening
                    eye = ImageEnhance.Brightness(eye).enhance(0.92 if k == 1 else 0.85)
            frames.append(eye)
        # 3 mouth frames, opening wider
        for m in frames_full:
            frames.append(m.crop((mxo, myo, mxo + mW, myo + mH)))
        write_sti(os.path.join(a.out, sub, f"{a.face}.sti"), frames, a.preview)
        if sub == "":
            offsets = (exo, eyo, mxo, myo)

    print(f'imp.json: "eyes": [ {offsets[0]}, {offsets[1]} ], "mouth": [ {offsets[2]}, {offsets[3]} ]')


if __name__ == "__main__":
    main()
