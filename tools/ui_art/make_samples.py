"""Draws the sample pictures for drawn interfaces in the «Интерфейс» tab.

    python tools/ui_art/make_samples.py   # needs Pillow and numpy

Writes ui/art/*.png: frame pictures cut into nine parts (the slice sizes are in
ui/art/art.json), tiling textures and mask shapes. The editor copies them into a
game's pictures/интерфейс/ the first time the tab opens.
"""
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

OUT = os.path.join(os.path.dirname(__file__), "..", "..", "ui", "art")
rng = np.random.default_rng(7)


def tileable_noise(size, scale, octaves=4):
    """Value noise that wraps around, 0..1."""
    out = np.zeros((size, size))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        cells = max(2, int(scale * 2 ** o))
        grid = rng.random((cells, cells))
        ys = np.linspace(0, cells, size, endpoint=False)
        y0 = np.floor(ys).astype(int)
        t = ys - y0
        t = t * t * (3 - 2 * t)
        y1 = (y0 + 1) % cells
        a = grid[y0][:, y0] * (1 - t)[None, :] + grid[y0][:, y1] * t[None, :]
        b = grid[y1][:, y0] * (1 - t)[None, :] + grid[y1][:, y1] * t[None, :]
        out += amp * (a * (1 - t)[:, None] + b * t[:, None])
        total += amp
        amp *= 0.5
    return out / total


def colorize(noise, dark, light):
    d, l = np.array(dark, float), np.array(light, float)
    rgb = d[None, None, :] + (l - d)[None, None, :] * noise[:, :, None]
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def parchment(size=128):
    n = tileable_noise(size, 3, 5)
    img = colorize(n, (196, 168, 120), (240, 224, 186))
    fibres = tileable_noise(size, 12, 2)
    arr = np.array(img).astype(float)
    arr[:, :, :3] -= (fibres[:, :, None] > 0.62) * 14
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")


def stone(size=128):
    n = tileable_noise(size, 4, 5)
    img = colorize(n, (70, 72, 78), (128, 130, 138))
    d = ImageDraw.Draw(img)
    # Bricks, wrapping at the edges.
    row_h = size // 4
    for r in range(4):
        y = r * row_h
        offset = (size // 4) if r % 2 else 0
        for x in range(0, size * 2, size // 2):
            for dx in (-size, 0):
                bx = x + offset + dx
                d.line([(bx, y), (bx, y + row_h)], fill=(40, 40, 46, 255), width=3)
        d.line([(0, y), (size, y)], fill=(40, 40, 46, 255), width=3)
    return img


def wood_frame(size=96, edge=32):
    n = tileable_noise(size, 2, 3)
    grain = np.sin(np.linspace(0, 26, size))[None, :] * 0.25 + n * 0.75
    img = colorize(grain, (92, 52, 22), (168, 104, 52))
    inner = parchment(size - 2 * edge + 8).crop((0, 0, size - 2 * edge, size - 2 * edge))
    img.paste(inner, (edge, edge))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, size - 1, size - 1], outline=(50, 26, 10, 255), width=3)
    d.rectangle([edge - 4, edge - 4, size - edge + 3, size - edge + 3], outline=(60, 32, 12, 255), width=3)
    for cx, cy in [(edge // 2, edge // 2), (size - edge // 2 - 1, edge // 2), (edge // 2, size - edge // 2 - 1),
                   (size - edge // 2 - 1, size - edge // 2 - 1)]:
        d.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], fill=(150, 150, 160, 255), outline=(40, 40, 48, 255), width=2)
        d.ellipse([cx - 4, cy - 6, cx + 2, cy], fill=(220, 220, 230, 255))
    return img


def stone_frame(size=96, edge=28):
    img = stone(size)
    d = ImageDraw.Draw(img)
    d.rectangle([edge, edge, size - edge - 1, size - edge - 1], fill=(28, 30, 36, 235))
    d.rectangle([edge - 3, edge - 3, size - edge + 2, size - edge + 2], outline=(170, 150, 90, 255), width=3)
    d.rectangle([0, 0, size - 1, size - 1], outline=(30, 30, 34, 255), width=3)
    for cx, cy in [(edge // 2, edge // 2), (size - edge // 2 - 1, edge // 2), (edge // 2, size - edge // 2 - 1),
                   (size - edge // 2 - 1, size - edge // 2 - 1)]:
        d.regular_polygon((cx, cy, 9), 4, rotation=45, fill=(200, 170, 90, 255), outline=(80, 60, 20, 255))
    return img


def button(w=96, h=48, r=14, top=(242, 196, 92), bottom=(196, 132, 40)):
    grad = np.linspace(0, 1, h)[:, None, None]
    rgb = np.array(top, float)[None, None, :] * (1 - grad) + np.array(bottom, float)[None, None, :] * grad
    rgb = np.repeat(rgb, w, axis=1)
    body = Image.fromarray(rgb.astype(np.uint8), "RGB").convert("RGBA")
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], r, fill=255)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    img.paste(body, (0, 0), mask)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, w - 1, h - 1], r, outline=(110, 64, 16, 255), width=3)
    d.rounded_rectangle([4, 4, w - 5, h // 2], r - 4, outline=(255, 236, 170, 200), width=2)
    return img


def blot(size=128):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = size / 2
    pts = []
    for i in range(48):
        a = i / 48 * 2 * math.pi
        rad = size * 0.36 * (1 + 0.18 * math.sin(a * 5 + 1) + 0.08 * math.sin(a * 11))
        pts.append((c + rad * math.cos(a), c + rad * math.sin(a)))
    d.polygon(pts, fill=(0, 0, 0, 255))
    for a, dist, r in [(0.3, 0.46, 7), (2.1, 0.44, 5), (3.9, 0.47, 6), (5.2, 0.43, 4)]:
        d.ellipse([c + size * dist * math.cos(a) - r, c + size * dist * math.sin(a) - r,
                   c + size * dist * math.cos(a) + r, c + size * dist * math.sin(a) + r], fill=(0, 0, 0, 255))
    return img.filter(ImageFilter.GaussianBlur(0.8))


def torn(w=256, h=128):
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    top = [(x, 8 + 6 * math.sin(x * 0.21) + rng.random() * 6) for x in range(0, w + 1, 4)]
    bottom = [(x, h - 8 - 6 * math.sin(x * 0.17 + 2) - rng.random() * 6) for x in range(w, -1, -4)]
    d.polygon(top + bottom, fill=(0, 0, 0, 255))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    pictures = {
        "рамка_дерево.png": (wood_frame(), {"slice": [32, 32, 32, 32]}),
        "рамка_камень.png": (stone_frame(), {"slice": [28, 28, 28, 28]}),
        "кнопка_золото.png": (button(), {"slice": [16, 16, 16, 16]}),
        "кнопка_синяя.png": (button(top=(120, 170, 240), bottom=(52, 92, 170)), {"slice": [16, 16, 16, 16]}),
        "пергамент.png": (parchment(), {"tile": True}),
        "камень.png": (stone(), {"tile": True}),
        "клякса.png": (blot(), {"mask": True}),
        "рваный_край.png": (torn(), {"mask": True}),
    }
    info = {}
    for name, (img, meta) in pictures.items():
        img.save(os.path.join(OUT, name), optimize=True)
        info[name] = meta
    with open(os.path.join(OUT, "art.json"), "w", encoding="utf-8") as f:
        json.dump({"pictures": info}, f, ensure_ascii=False, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
