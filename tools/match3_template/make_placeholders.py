#!/usr/bin/env python3
"""Placeholders for the pictures of the template «Три в ряд» (step 14.3c), and its sound.

Each picture has the size and the frames the brief asks for (/mnt/project-files/step14/14.3c-ТЗ-картинки.md, in the
repo: games/templates/README.md), so the template plays and its checks run before the art is drawn. Every one is
marked as a placeholder: a magenta frame around each frame and the letter «З» in its corner (the cells of tiles.png
only the letter: they must stay whole). They are not the art and are replaced by it file for file; then
`forge_editor --make-template match3 games/match3` makes the template's game again.

    python3 tools/match3_template/make_placeholders.py

writes games/templates/match3/assets/картинки/*.png (six gems of 4 frames 32 × 32, three HUD icons 32 × 32),
games/templates/match3/assets/звуки/звон.wav (the gems' sound, the one of the example games/examples/screen-sound),
each with its .meta, and games/templates/match3/tiles.png (the cells: 96 × 32, ids 256, 257, 258). A .meta keeps the
id «Ресурсы» know the file by: made from the file's place, so it is the same each time. Needs Pillow.

The middle of every gem's frame is its own colour, opaque (what the game's pixel checks look for); each gem has a
shape of its own (the brief: told apart without colour).
"""
import json
import math
import pathlib
import shutil
import sys
import uuid

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "games" / "templates" / "match3"
OUT = TEMPLATE / "assets"

MAGENTA = (255, 0, 255, 255)
INK = (43, 29, 46, 255)  # #2B1D2E, the brief's outline
CLEAR = (0, 0, 0, 0)

LETTER = ["###", "..#", ".##", "..#", "###"]  # «З», 3 × 5
SIDE = 32


def letter(img, x0, y0):
    px = img.load()
    for row, line in enumerate(LETTER):
        for col, c in enumerate(line):
            if c == "#":
                px[x0 + col, y0 + row] = MAGENTA


def mark(img, x0, y0, x1, y1):
    """A magenta frame x0..x1, y0..y1 (inclusive) and «З» inside its top left corner."""
    px = img.load()
    for x in range(x0, x1 + 1):
        px[x, y0] = MAGENTA
        px[x, y1] = MAGENTA
    for y in range(y0, y1 + 1):
        px[x0, y] = MAGENTA
        px[x1, y] = MAGENTA
    letter(img, x0 + 2, y0 + 2)


def lighter(c, k):
    return tuple(min(255, int(round(v + (255 - v) * k))) for v in c[:3]) + (255,)


# The six gems: the colour of the middle and the shape, as the brief has them.
GEMS = [
    ("рубин.png", (200, 40, 60), "round"),
    ("изумруд.png", (40, 170, 80), "octagon"),
    ("сапфир.png", (40, 90, 210), "drop"),
    ("топаз.png", (235, 190, 40), "rhombus"),
    ("аметист.png", (150, 70, 200), "hexagon"),
    ("алмаз.png", (205, 235, 250), "brilliant"),
]


def inside(shape, x, y, r):
    """Whether the point (from the frame's middle, in pixels) is in a gem of that shape and half size r."""
    ax, ay = abs(x), abs(y)
    if shape == "round":
        return x * x + y * y <= r * r
    if shape == "octagon":  # a square with its corners cut
        return ax <= r and ay <= r and ax + ay <= r * 1.45
    if shape == "drop":  # a point up, round below
        if y >= 0:
            return x * x + y * y <= r * r
        return ax <= r * (1 + y / (r * 1.05)) if y >= -r * 1.05 else False
    if shape == "rhombus":
        return ax + ay <= r * 1.05
    if shape == "hexagon":
        return ay <= r * 0.87 and ax * 0.87 + ay * 0.5 <= r * 0.87
    if shape == "brilliant":  # a wide top, a point down
        if y <= -r * 0.3:
            return ax <= r and y >= -r * 0.8
        return ax <= r * (1 - (y + r * 0.3) / (r * 1.3))
    raise ValueError(shape)


def gem_frame(colour, shape, frame):
    """One 32 × 32 frame: 0 lies, 1 flashes (lighter), 2 cracked and smaller, 3 sparks. The stone keeps to 2..29."""
    f = Image.new("RGBA", (SIDE, SIDE), CLEAR)
    px = f.load()
    r = {0: 13.0, 1: 13.0, 2: 8.5, 3: 0}[frame]
    fill = (colour + (255,)) if frame != 1 else lighter(colour, 0.45)
    c = SIDE / 2
    if frame == 3:
        # What is left: a small core of its colour and sparks round it.
        for y in range(SIDE):
            for x in range(SIDE):
                dx, dy = x + 0.5 - c, y + 0.5 - c
                if abs(dx) <= 3 and abs(dy) <= 3:
                    px[x, y] = INK if max(abs(dx), abs(dy)) > 2 else lighter(colour, 0.2)
        for k in range(6):
            a = k * math.pi / 3
            sx, sy = int(c + math.cos(a) * 9), int(c + math.sin(a) * 9)
            for dx in range(2):
                for dy in range(2):
                    px[sx + dx, sy + dy] = lighter(colour, 0.6)
        return f
    for y in range(SIDE):
        for x in range(SIDE):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            if not inside(shape, dx, dy, r):
                continue
            edge = not all(inside(shape, dx + ox, dy + oy, r) for ox, oy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if edge:
                px[x, y] = INK
            elif dx < -2 and dy < -2 and dx > -r * 0.7 and dy > -r * 0.7 and (dx + dy) > -r * 0.9:
                px[x, y] = lighter(fill, 0.5)  # the light from up left
            else:
                px[x, y] = fill
    if frame == 2:  # cracks
        for i in range(-5, 6):
            px[int(c + i), int(c + i * 0.4)] = INK if abs(i) > 1 else fill
    return f


def gem(colour, shape):
    """128 × 32: four frames of 32 × 32 side by side, each marked."""
    img = Image.new("RGBA", (SIDE * 4, SIDE), CLEAR)
    for i in range(4):
        f = gem_frame(colour, shape, i)
        mark(f, 0, 0, SIDE - 1, SIDE - 1)
        img.paste(f, (i * SIDE, 0))
    return img


def icon(kind):
    """32 × 32 HUD icons: «ходы» two arrows the other way each, «цель» a target, «очки» a star."""
    f = Image.new("RGBA", (SIDE, SIDE), CLEAR)
    px = f.load()
    gold, white, red = (250, 205, 60, 255), (235, 235, 245, 255), (210, 50, 50, 255)
    c = SIDE / 2
    for y in range(SIDE):
        for x in range(SIDE):
            dx, dy = x + 0.5 - c, y + 0.5 - c
            if kind == "moves":
                # → on top (shaft 6..21, head 21..27), ← below (head 4..10, shaft 10..25).
                right = (9 <= y <= 12 and 6 <= x <= 21) or (21 <= x <= 27 and abs(y + 0.5 - 11) <= (27.5 - x) * 0.9)
                left = (19 <= y <= 22 and 10 <= x <= 25) or (4 <= x <= 10 and abs(y + 0.5 - 21) <= (x - 3.5) * 0.9)
                if right or left:
                    px[x, y] = white
            elif kind == "target":
                d = math.hypot(dx, dy)
                if d <= 13:
                    px[x, y] = red if (int(d) // 3) % 2 == 0 else white
            else:  # a star of five points
                a = (math.atan2(dy, dx) + math.pi / 2) % (2 * math.pi / 5) / (2 * math.pi / 5)
                if math.hypot(dx, dy) <= 5.5 + 7.5 * abs(1 - 2 * a):
                    px[x, y] = gold
    mark(f, 0, 0, SIDE - 1, SIDE - 1)
    return f


def tiles():
    """96 × 32: «Клетка» (256), «Клетка тёмная» (257), both whole and seamless; «Камень» (258) on a clear ground."""
    img = Image.new("RGBA", (SIDE * 3, SIDE), CLEAR)
    px = img.load()
    for n, (fill, edge) in enumerate((((96, 90, 112), (80, 74, 96)), ((74, 68, 90), (62, 56, 78)))):
        for y in range(SIDE):
            for x in range(SIDE):
                border = x in (0, SIDE - 1) or y in (0, SIDE - 1)
                px[n * SIDE + x, y] = (edge if border else fill) + (255,)
        letter(img, n * SIDE + 3, 3)
    c = SIDE / 2
    for y in range(SIDE):
        for x in range(SIDE):
            d = math.hypot((x + 0.5 - c) / 1.15, y + 0.5 - c - 1)
            if d <= 12:
                px[2 * SIDE + x, y] = INK if d > 10.8 else ((128, 116, 104, 255) if (x + y) % 7 else (110, 98, 88, 255))
    letter(img, 2 * SIDE + 3, 3)
    return img


def meta(file):
    """The file's .meta as «Ресурсы» write it, its id made from where it is in the template."""
    rel = file.relative_to(OUT).as_posix()
    guid = uuid.uuid5(uuid.NAMESPACE_URL, "forge:templates/match3/assets/" + rel)
    text = json.dumps({"$type": "forge::assets::AssetMeta", "id": str(guid), "tags": []}, ensure_ascii=False, indent=2)
    pathlib.Path(str(file) + ".meta").write_text(text + "\n", encoding="utf-8", newline="\n")


def main():
    pictures = OUT / "картинки"
    sounds = OUT / "звуки"
    pictures.mkdir(parents=True, exist_ok=True)
    sounds.mkdir(parents=True, exist_ok=True)
    made = {name: gem(colour, shape) for name, colour, shape in GEMS}
    made.update({"ходы.png": icon("moves"), "цель.png": icon("target"), "очки.png": icon("score")})
    for name, img in made.items():
        img.save(pictures / name, optimize=True)
        meta(pictures / name)
    ring = sounds / "звон.wav"
    shutil.copyfile(ROOT / "games" / "examples" / "screen-sound" / "sounds" / "звон.wav", ring)
    meta(ring)
    tiles().save(TEMPLATE / "tiles.png", optimize=True)
    print(f"{len(made)} заглушек, плитки и звук записаны в {TEMPLATE}", file=sys.stderr)


if __name__ == "__main__":
    main()
