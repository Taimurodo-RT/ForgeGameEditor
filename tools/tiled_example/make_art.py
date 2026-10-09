"""Pictures and music of games/examples/tiled (the sources of the Tiled map).

Drawn by arithmetic alone (nothing random), so the pixels are the same on every computer:
    python tools/tiled_example/make_art.py
Writes into games/examples/tiled/Карта Tiled/. Needs Pillow.
"""
import os
import struct
import sys

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "games", "examples", "tiled", "Карта Tiled")
PX = 16


def tile(draw):
    """A 16 × 16 RGBA picture: draw(x, y) -> (r, g, b, a)."""
    img = Image.new("RGBA", (PX, PX))
    img.putdata([draw(x, y) for y in range(PX) for x in range(PX)])
    return img


def noise(x, y, seed):
    """0..255, the same for the same x, y, seed."""
    v = (x * 374761393 + y * 668265263 + seed * 2246822519) & 0xFFFFFFFF
    v = ((v ^ (v >> 13)) * 1274126177) & 0xFFFFFFFF
    return (v ^ (v >> 16)) & 0xFF


def shade(c, d):
    return tuple(max(0, min(255, v + d)) for v in c[:3]) + (255,)


DIRT = (134, 90, 52)
GRASS = (88, 160, 62)
STONE = (128, 128, 136)
DARK = (70, 66, 78)
BRICK = (168, 74, 58)
MORTAR = (206, 196, 176)
WOOD = (176, 128, 70)


def dirt(x, y):
    return shade(DIRT, noise(x, y, 1) % 24 - 12)


def grass(x, y):
    top = 4 + (noise(x, 0, 2) % 3)
    return shade(GRASS, noise(x, y, 3) % 20 - 10) if y < top else dirt(x, y)


def stone(x, y):
    crack = (x + 2 * y) % 11 == 0 and 3 < x < 13
    return shade(STONE, -40 if crack else noise(x, y, 4) % 18 - 9)


def dark(x, y):
    return shade(DARK, noise(x, y, 5) % 14 - 7)


def brick(x, y):
    row = y // 4
    if y % 4 == 3 or (x + (row % 2) * 8) % 16 == 15:
        return MORTAR + (255,)
    return shade(BRICK, noise(x, y, 6) % 16 - 8)


def planks(x, y):
    if y % 8 == 7:
        return shade(WOOD, -60)
    if (x == 3 and y < 8) or (x == 11 and y >= 8):
        return shade(WOOD, -45)
    return shade(WOOD, noise(x, y // 2, 7) % 14 - 7)


def corner(x, y):
    # Grass on the top and the left only: every flip of it looks different.
    if y < 5 or x < 4:
        return shade(GRASS, noise(x, y, 8) % 20 - 10)
    return dirt(x, y)


def ore(x, y):
    # Stone with a gold vein from the top left down to the right, and a red dot at the top right.
    if x >= 12 and y <= 3:
        return (220, 40, 40, 255)
    if abs(y - x // 2 - 2) <= 1 and x < 12:
        return (232, 196, 64, 255)
    return stone(x, y)


def cave_wall(x, y):
    return shade(DARK, -18 + noise(x // 2, y // 2, 9) % 12)


def brick_wall(x, y):
    return shade(brick(x, y), -70)


def window(x, y):
    if 3 <= x <= 12 and 3 <= y <= 12:
        if x == 7 or x == 8 or y == 7 or y == 8:
            return (92, 60, 34, 255)
        return (150, 200, 235, 255)
    return brick_wall(x, y)


def arrow(x, y):
    # A wall tile with an arrow pointing right in its upper half: flips are easy to see.
    if 6 <= y <= 7 and 2 <= x <= 12:
        return (240, 240, 240, 255)
    if 3 <= y <= 10 and x >= 9 and abs(y - 6.5) <= (13 - x) * 0.9:
        return (240, 240, 240, 255)
    return cave_wall(x, y)


def ladder(x, y):
    if x in (3, 4, 11, 12) or y % 5 == 2:
        return shade(WOOD, -20)
    return (0, 0, 0, 0)


GROUND = [grass, dirt, stone, dark, brick, planks, corner, ore]
WALLS = [cave_wall, brick_wall, window, arrow, ladder]


def atlas(tiles, columns, margin, spacing, guide):
    rows = (len(tiles) + columns - 1) // columns
    w = 2 * margin + columns * PX + (columns - 1) * spacing
    h = 2 * margin + rows * PX + (rows - 1) * spacing
    img = Image.new("RGBA", (w, h), guide)
    for i, t in enumerate(tiles):
        x = margin + (i % columns) * (PX + spacing)
        y = margin + (i // columns) * (PX + spacing)
        img.paste(tile(t), (x, y))
    return img


# The sky: a see-through colour (#ff00ff) around clouds, as old tilesets have it.
MAGENTA = (255, 0, 255, 255)


def cloud(cx, cy, r):
    def draw(x, y):
        if (x - cx) ** 2 * 4 + (y - cy) ** 2 * 9 < r * r * 9:
            return (250, 250, 252, 255) if y < cy else (214, 222, 236, 255)
        return MAGENTA
    return draw


def sun(x, y):
    d = (x - 7.5) ** 2 + (y - 7.5) ** 2
    if d < 20:
        return (255, 214, 64, 255)
    if d < 36 and (x + y) % 2 == 0:
        return (255, 236, 140, 255)
    return MAGENTA


SKY = [cloud(5, 9, 5), cloud(10, 8, 6), sun, lambda x, y: MAGENTA]


# Little things over the background, with alpha.
def flower(x, y):
    if y >= 9 and x in (7, 8):
        return (60, 140, 50, 255)
    if 4 <= y <= 8 and 5 <= x <= 10 and (x - 7.5) ** 2 + (y - 6) ** 2 < 7:
        return (230, 90, 160, 255) if (x + y) % 3 else (255, 220, 80, 255)
    return (0, 0, 0, 0)


def mushroom(x, y):
    if y >= 10 and 6 <= x <= 9:
        return (236, 226, 200, 255)
    if 5 <= y <= 9 and (x - 7.5) ** 2 / 25 + (y - 9) ** 2 / 16 <= 1:
        return (200, 40, 40, 255) if (x * 3 + y) % 5 else (255, 255, 255, 255)
    return (0, 0, 0, 0)


def vine(x, y):
    if abs(x - 8 - (2 if y % 8 < 4 else -2)) <= 1:
        return (50, 120, 60, 200)
    if (y % 8 == 2 and 9 <= x <= 12) or (y % 8 == 6 and 4 <= x <= 7):
        return (80, 170, 80, 230)
    return (0, 0, 0, 0)


def moss(x, y):
    return (70, 150, 70, 150) if y < 4 + noise(x, 0, 11) % 4 else (0, 0, 0, 0)


DECOR = [flower, mushroom, vine, moss]


# The things: pictures of their own (a collection of images).
def chest(x, y):
    if y < 4 or x < 1 or x > 14:
        return (0, 0, 0, 0)
    if y == 8 or x in (1, 14) or y in (4, 15):
        return (90, 56, 28, 255)
    if 7 <= x <= 8 and 7 <= y <= 10:
        return (240, 200, 70, 255)
    return (164, 108, 52, 255)


def sign(x, y):
    if 2 <= y <= 9 and 1 <= x <= 14:
        if y in (4, 7) and 3 <= x <= 12:
            return (70, 44, 24, 255)
        return (196, 150, 90, 255)
    if y > 9 and 7 <= x <= 8:
        return (110, 74, 40, 255)
    return (0, 0, 0, 0)


def lamp_tall():
    img = Image.new("RGBA", (PX, 2 * PX))
    data = []
    for y in range(2 * PX):
        for x in range(PX):
            if y < 9 and 4 <= x <= 11 and (x - 7.5) ** 2 + (y - 5) ** 2 < 14:
                data.append((255, 220, 120, 255) if (x - 7.5) ** 2 + (y - 5) ** 2 < 6 else (60, 60, 70, 255))
            elif y >= 9 and 7 <= x <= 8:
                data.append((60, 60, 70, 255))
            elif y >= 29 and 4 <= x <= 11:
                data.append((60, 60, 70, 255))
            else:
                data.append((0, 0, 0, 0))
    img.putdata(data)
    return img


def wav(path):
    """Two seconds of a tune going up (22 kHz mono, 16 bit), from whole numbers only."""
    rate, note = 22050, 22050 // 4
    hz = [262, 294, 330, 349, 392, 440, 494, 523]
    frames = bytearray()
    for k, f in enumerate(hz):
        for t in range(note):
            phase = (t * f * 65536 // rate) & 0xFFFF
            tri = phase * 2 - 32768 if phase < 32768 else 98304 - phase * 2
            up = min(t, note // 50)
            down = min(note - t, note // 3)
            env = min(up * 1000 // (note // 50), down * 1000 // (note // 3))
            frames += struct.pack("<h", (tri // 4) * env // 1000)
    head = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    with open(path, "wb") as f:
        f.write(head + b"data" + struct.pack("<I", len(frames)) + frames)


def main():
    sets = os.path.join(ROOT, "наборы")
    things = os.path.join(ROOT, "картинки")
    music = os.path.join(ROOT, "музыка")
    for d in (sets, things, music):
        os.makedirs(d, exist_ok=True)
    # 8 + 5 tiles, 8 in a row, a 1 px margin and 2 px between them (the guide colour shows when it is cut wrong).
    atlas(GROUND + WALLS, 8, 1, 2, (255, 0, 128, 255)).save(os.path.join(sets, "Земля и камень.png"))
    atlas(SKY, 4, 0, 0, MAGENTA).convert("RGB").save(os.path.join(ROOT, "небо.png"))
    atlas(DECOR, 4, 0, 0, (0, 0, 0, 0)).save(os.path.join(sets, "декор.png"))
    tile(chest).save(os.path.join(things, "сундук.png"))
    tile(sign).save(os.path.join(things, "табличка.png"))
    lamp_tall().save(os.path.join(things, "фонарь.png"))
    wav(os.path.join(music, "пещера.wav"))
    print("written:", ROOT)


if __name__ == "__main__":
    sys.exit(main())
