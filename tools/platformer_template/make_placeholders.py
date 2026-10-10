#!/usr/bin/env python3
"""Placeholders for the pictures of the template «Платформер» (step 14.2d).

Each has the size, the frames and the anchor the brief asks for
(/mnt/project-files/step14/14.2d-ТЗ-картинки.md, in the repo: games/templates/README.md), so the template plays and its
checks run before the art is drawn. Every one is marked as a placeholder: a magenta frame around the figure and the
letter «З» in its corner (tiles: the letter only). They are not the art and are replaced by it file for file.

    python3 tools/platformer_template/make_placeholders.py

writes games/templates/platformer/assets/картинки/*.png (the template's «Ресурсы») and
games/templates/platformer/tiles.png (the levels' tiles). Needs Pillow.
"""
import pathlib
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "games" / "templates" / "platformer"

MAGENTA = (255, 0, 255, 255)
INK = (43, 29, 46, 255)  # the outline colour of the brief
CLEAR = (0, 0, 0, 0)

LETTER = ["###", "..#", ".##", "..#", "###"]  # «З», 3 × 5


def mark(img, x0, y0, x1, y1, frame=True):
    """A magenta frame x0..x1, y0..y1 (inclusive) and «З» inside its top left corner."""
    px = img.load()
    if frame:
        for x in range(x0, x1 + 1):
            px[x, y0] = MAGENTA
            px[x, y1] = MAGENTA
        for y in range(y0, y1 + 1):
            px[x0, y] = MAGENTA
            px[x1, y] = MAGENTA
    for row, line in enumerate(LETTER):
        for col, c in enumerate(line):
            if c == "#":
                px[x0 + 2 + col, y0 + 2 + row] = MAGENTA


def rect(img, x0, y0, x1, y1, color):
    px = img.load()
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px[x, y] = color


def outline(img, x0, y0, x1, y1, fill, ink=INK):
    rect(img, x0, y0, x1, y1, ink)
    if x1 - x0 >= 2 and y1 - y0 >= 2:
        rect(img, x0 + 1, y0 + 1, x1 - 1, y1 - 1, fill)


def disc(img, cx, cy, r, fill, ink=INK):
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            d = (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
            if d <= r * r:
                px[x, y] = ink if d > (r - 1.2) ** 2 else fill


def hero():
    """128 × 64: stands, step 1, step 2, in the air. Feet end on row 61, head from row 3, body in columns 4..27."""
    img = Image.new("RGBA", (128, 64), CLEAR)
    skin, shirt, pants, scarf, pack, boot = (240, 196, 150, 255), (70, 120, 200, 255), (90, 70, 50, 255), (210, 40, 40, 255), (150, 100, 50, 255), (60, 40, 30, 255)
    for f in range(4):
        ox = f * 32
        sub = Image.new("RGBA", (32, 64), CLEAR)
        outline(sub, 10, 3, 21, 16, skin)  # head
        rect(sub, 18, 8, 19, 9, INK)  # eye, looking right
        outline(sub, 9, 17, 22, 19, scarf)  # scarf
        outline(sub, 6, 20, 10, 36, pack)  # backpack
        outline(sub, 10, 19, 22, 40, shirt)  # body
        arm_y = 22 if f != 3 else 14
        outline(sub, 21, arm_y, 25, arm_y + 12, shirt)  # front arm (up in the air)
        legs = {0: ((11, 15), (17, 21)), 1: ((8, 12), (19, 23)), 2: ((13, 17), (15, 19)), 3: ((10, 14), (18, 22))}[f]
        bottom = 61 if f != 3 else 56
        for lx0, lx1 in legs:
            outline(sub, lx0, 41, lx1, bottom - 4, pants)
            outline(sub, lx0 - 1, bottom - 3, lx1 + 1, bottom, boot)
        mark(sub, 3, 2, 28, 61)
        img.paste(sub, (ox, 0))
    return img


def walker(shell, spiky):
    """64 × 32, two steps; feet end on row 28, figure in columns 5..26, top not above row 4."""
    img = Image.new("RGBA", (64, 32), CLEAR)
    for f in range(2):
        sub = Image.new("RGBA", (32, 32), CLEAR)
        outline(sub, 6, 12, 25, 24, shell)  # body
        rect(sub, 21, 15, 22, 16, INK)  # eye, looking right
        if spiky:
            px = sub.load()
            for i, x in enumerate(range(7, 25, 3)):
                for k in range(4 + (i % 2)):
                    px[x, 11 - k] = INK
                    px[x + 1, 11 - k] = (230, 230, 230, 255) if k < 3 else INK
        else:
            outline(sub, 8, 6, 22, 12, (min(shell[0] + 40, 255), min(shell[1] + 40, 255), min(shell[2] + 40, 255), 255))
        legs = (8, 13, 18, 23) if f == 0 else (10, 15, 20, 21)
        for x in legs:
            outline(sub, x, 25, x + 2, 28, INK)
        mark(sub, 5, 4, 26, 28)
        img.paste(sub, (f * 32, 0))
    return img


def coin():
    img = Image.new("RGBA", (32, 32), CLEAR)
    disc(img, 16, 16, 13.5, (240, 190, 40, 255))
    rect(img, 13, 9, 15, 22, (255, 230, 120, 255))
    mark(img, 1, 1, 30, 30)
    return img


def spikes(cells):
    img = Image.new("RGBA", (32 * cells, 32), CLEAR)
    px = img.load()
    rect(img, 0, 26, 32 * cells - 1, 31, (110, 110, 120, 255))
    rect(img, 0, 26, 32 * cells - 1, 26, INK)
    for c in range(cells):
        for s in range(4):
            cx = c * 32 + 4 + s * 8
            for y in range(3, 26):
                half = (y - 3) * 4 // 23
                for x in range(cx - half, cx + half + 1):
                    px[x, y] = INK if x in (cx - half, cx + half) else (200, 205, 215, 255)
    mark(img, 0, 0, 32 * cells - 1, 31)
    return img


def flag():
    img = Image.new("RGBA", (32, 64), CLEAR)
    outline(img, 6, 2, 9, 63, (150, 100, 50, 255))  # the pole, its base on row 63
    outline(img, 10, 4, 28, 20, (220, 50, 40, 255))
    rect(img, 11, 12, 27, 19, (250, 200, 40, 255))
    mark(img, 0, 0, 31, 63)
    return img


def sign():
    img = Image.new("RGBA", (32, 32), CLEAR)
    outline(img, 14, 16, 17, 31, (150, 100, 50, 255))  # the leg, standing on row 31
    outline(img, 3, 5, 28, 17, (190, 140, 80, 255))
    px = img.load()
    for x in range(8, 22):
        px[x, 11] = INK
    for k in range(4):
        px[21 - k, 11 - k] = INK
        px[21 - k, 11 + k] = INK
    mark(img, 0, 0, 31, 31)
    return img


def heart():
    img = Image.new("RGBA", (32, 32), CLEAR)
    disc(img, 11, 12, 7.5, (220, 40, 50, 255))
    disc(img, 21, 12, 7.5, (220, 40, 50, 255))
    px = img.load()
    for y in range(12, 28):
        half = max(0, 15 - (y - 12))
        for x in range(16 - half, 16 + half):
            px[x, y] = (220, 40, 50, 255)
    for y in range(12, 28):
        half = max(0, 15 - (y - 12))
        px[16 - half, y] = INK
        px[min(31, 16 + half), y] = INK
    rect(img, 8, 9, 10, 11, (255, 170, 170, 255))
    mark(img, 0, 0, 31, 31)
    return img


def tiles():
    """512 × 32: 16 tiles in one row, ids 256..271 in tiles.json order. Solid ones have their top row opaque."""
    img = Image.new("RGBA", (512, 32), CLEAR)
    grass, grass_dark, dirt, dirt_dark, wood, wood_dark, stone, stone_dark = (
        (90, 180, 70, 255), (60, 140, 50, 255), (150, 100, 60, 255), (120, 80, 50, 255),
        (180, 130, 70, 255), (130, 90, 50, 255), (140, 140, 150, 255), (100, 100, 110, 255))

    def grassy(sub, left, right):
        rect(sub, 0, 0, 31, 31, dirt)
        rect(sub, 0, 0, 31, 7, grass)
        rect(sub, 0, 8, 31, 9, grass_dark)
        if left:
            rect(sub, 0, 0, 0, 31, INK)
        if right:
            rect(sub, 31, 0, 31, 31, INK)
        rect(sub, 0, 0, 31, 0, (40, 110, 40, 255))

    for i in range(16):
        sub = Image.new("RGBA", (32, 32), CLEAR)
        if i in (0, 1, 2):
            grassy(sub, i == 1, i == 2)
        elif i in (3, 4, 5):
            rect(sub, 0, 0, 31, 31, dirt)
            for y in range(4, 30, 9):
                rect(sub, (y * 3) % 24, y, (y * 3) % 24 + 4, y + 1, dirt_dark)
            if i == 4:
                rect(sub, 0, 0, 0, 31, INK)
            if i == 5:
                rect(sub, 31, 0, 31, 31, INK)
        elif i in (6, 7, 8):
            rect(sub, 0, 0, 31, 31, wood_dark)
            rect(sub, 0, 0, 31, 11, wood)
            rect(sub, 0, 0, 31, 0, INK)
            rect(sub, 0, 11, 31, 11, INK)
            for x in range(4, 32, 8):
                rect(sub, x, 12, x + 1, 31, wood)
            if i == 6:
                rect(sub, 0, 0, 0, 31, INK)
            if i == 8:
                rect(sub, 31, 0, 31, 31, INK)
        elif i == 9:
            rect(sub, 0, 0, 31, 31, stone)
            rect(sub, 0, 0, 31, 0, INK)
            rect(sub, 0, 15, 31, 15, stone_dark)
            rect(sub, 15, 0, 15, 15, stone_dark)
            rect(sub, 7, 16, 7, 31, stone_dark)
        elif i == 10:
            rect(sub, 0, 0, 31, 31, dirt_dark)
        elif i == 11:
            disc(sub, 16, 20, 11.5, (60, 150, 60, 255))
        elif i == 12:
            px = sub.load()
            for x in (5, 12, 19, 26):
                for y in range(18, 32):
                    px[x, y] = (60, 150, 60, 255)
                rect(sub, x - 1, 15, x + 1, 17, (240, 220, 80, 255) if x % 2 else (230, 90, 140, 255))
        elif i in (13, 14):
            disc(sub, 32 if i == 13 else 0, 20, 12.5, (250, 250, 255, 255), ink=(200, 210, 230, 255))
        elif i == 15:
            disc(sub, 16, 26, 10.5, (150, 160, 175, 255), ink=(120, 130, 145, 255))
        mark(sub, 0, 0, 31, 31, frame=False)
        img.paste(sub, (i * 32, 0))
    return img


def main():
    pictures = OUT / "assets" / "картинки"
    pictures.mkdir(parents=True, exist_ok=True)
    made = {
        "герой.png": hero(),
        "жук.png": walker((60, 140, 90, 255), False),
        "еж.png": walker((120, 90, 70, 255), True),
        "монета.png": coin(),
        "шипы.png": spikes(1),
        "ряд_шипов.png": spikes(3),
        "флаг.png": flag(),
        "указатель.png": sign(),
        "сердце.png": heart(),
    }
    for name, img in made.items():
        img.save(pictures / name, optimize=True)
    tiles().save(OUT / "tiles.png", optimize=True)
    print(f"{len(made) + 1} заглушек записаны в {OUT}", file=sys.stderr)


if __name__ == "__main__":
    main()
