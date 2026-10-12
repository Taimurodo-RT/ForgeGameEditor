#!/usr/bin/env python3
"""Placeholders for the pictures of the template «RPG сверху» (step 14.4b), and its sound.

Each picture has the size and the frames the brief asks for (/mnt/project-files/step14/14.4b-ТЗ-картинки.md, in the
repo: games/templates/README.md), so the template plays and its checks run before the art is drawn. Every one is
marked as a placeholder: a magenta frame around each frame and the letter «З» in its corner. They are not the art and
are replaced by it file for file; then `forge_editor --make-template rpg games/rpg` makes the template's game again.

    python3 tools/rpg_template/make_placeholders.py

writes games/templates/rpg/assets/картинки/*.png and games/templates/rpg/assets/звуки/звон.wav (the template's
«Ресурсы»: the coins' sound, the one of the example games/examples/screen-sound), each with its .meta. A .meta keeps
the id «Ресурсы» know the file by: made from the file's place, so it is the same each time. Needs Pillow.
"""
import json
import pathlib
import shutil
import sys
import uuid

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "games" / "templates" / "rpg" / "assets"

MAGENTA = (255, 0, 255, 255)
INK = (34, 30, 40, 255)
CLEAR = (0, 0, 0, 0)

LETTER = ["###", "..#", ".##", "..#", "###"]  # «З», 3 × 5


def mark(img, x0, y0, x1, y1):
    """A magenta frame x0..x1, y0..y1 (inclusive) and «З» inside its top left corner."""
    px = img.load()
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
    for y in range(max(0, y0), min(img.height - 1, y1) + 1):
        for x in range(max(0, x0), min(img.width - 1, x1) + 1):
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


def strip(frames):
    """Frames of one size side by side, each marked."""
    w, h = frames[0].size
    img = Image.new("RGBA", (w * len(frames), h), CLEAR)
    for i, f in enumerate(frames):
        mark(f, 0, 0, w - 1, h - 1)
        img.paste(f, (i * w, 0))
    return img


SKIN = (240, 196, 150, 255)


def person(shirt, pants, hat=None, step=0, beard=False):
    """48 × 64, seen from the front a little from above: feet end on row 61. step: 0 stands, 1 left foot out, 2 right."""
    f = Image.new("RGBA", (48, 64), CLEAR)
    outline(f, 17, 6, 30, 19, SKIN)  # head
    rect(f, 20, 12, 21, 13, INK)  # eyes
    rect(f, 26, 12, 27, 13, INK)
    if hat:
        outline(f, 14, 3, 33, 8, hat)
    if beard:
        outline(f, 19, 16, 28, 22, (220, 220, 220, 255))
    outline(f, 13, 20, 34, 44, shirt)  # body
    outline(f, 8, 22, 13, 40, shirt)  # arms
    outline(f, 34, 22, 39, 40, shirt)
    left = 46 + (3 if step == 1 else 0)
    right = 46 + (3 if step == 2 else 0)
    outline(f, 15, 44, 22, min(61, left + 12), pants)  # legs
    outline(f, 25, 44, 32, min(61, right + 12), pants)
    return f


def hero():
    """144 × 64: stands, step 1, step 2 (the module plays 1, 2 while he walks)."""
    shirt, pants, scarf = (70, 120, 200, 255), (90, 70, 50, 255), (210, 40, 40, 255)
    frames = []
    for step in range(3):
        f = person(shirt, pants, step=step)
        outline(f, 15, 19, 32, 22, scarf)
        frames.append(f)
    return strip(frames)


def villager(shirt, pants, hat=None, beard=False):
    return strip([person(shirt, pants, hat, beard=beard)])


def dog():
    """96 × 40: two frames, the tail up and down."""
    frames = []
    for wag in range(2):
        f = Image.new("RGBA", (48, 40), CLEAR)
        fur, dark = (150, 110, 70, 255), (100, 70, 45, 255)
        outline(f, 10, 16, 36, 30, fur)  # body
        outline(f, 30, 8, 43, 20, fur)  # head
        rect(f, 38, 12, 39, 13, INK)  # eye
        outline(f, 31, 4, 34, 9, dark)  # ear
        for x in (12, 18, 26, 32):
            outline(f, x, 30, x + 3, 37, dark)  # legs
        outline(f, 3, 10 if wag == 0 else 18, 10, 13 if wag == 0 else 21, dark)  # tail
        frames.append(f)
    return strip(frames)


def key():
    """48 × 48: a key lying in the middle of its cell."""
    f = Image.new("RGBA", (48, 48), CLEAR)
    gold = (236, 196, 40, 255)
    disc(f, 15, 24, 9, gold)
    disc(f, 15, 24, 4, CLEAR, ink=gold)
    outline(f, 23, 21, 41, 27, gold)
    outline(f, 35, 27, 38, 34, gold)
    outline(f, 29, 27, 32, 31, gold)
    return strip([f])


def coins():
    """48 × 48: three coins in a heap."""
    f = Image.new("RGBA", (48, 48), CLEAR)
    gold, dark = (250, 205, 60, 255), (190, 140, 30, 255)
    for i, (cx, cy) in enumerate(((17, 31), (31, 31), (24, 20))):
        disc(f, cx, cy, 10.5, gold if i != 1 else dark)
    return strip([f])


def chest():
    f = Image.new("RGBA", (48, 40), CLEAR)
    wood, band, lock = (150, 90, 40, 255), (90, 60, 30, 255), (236, 196, 40, 255)
    outline(f, 5, 8, 42, 20, wood)  # lid
    outline(f, 5, 20, 42, 37, wood)  # box
    for x in (12, 34):
        rect(f, x, 9, x + 1, 36, band)
    outline(f, 21, 17, 26, 25, lock)
    return strip([f])


def gate():
    f = Image.new("RGBA", (48, 64), CLEAR)
    stone, wood = (150, 150, 160, 255), (120, 80, 40, 255)
    outline(f, 2, 6, 11, 61, stone)  # posts
    outline(f, 36, 6, 45, 61, stone)
    outline(f, 2, 4, 45, 12, stone)  # lintel
    outline(f, 12, 13, 35, 61, wood)  # the doors
    rect(f, 23, 14, 24, 60, INK)
    return strip([f])


def meta(file):
    """The file's .meta as «Ресурсы» write it, its id made from where it is in the template."""
    rel = file.relative_to(OUT).as_posix()
    guid = uuid.uuid5(uuid.NAMESPACE_URL, "forge:templates/rpg/assets/" + rel)
    text = json.dumps({"$type": "forge::assets::AssetMeta", "id": str(guid), "tags": []}, ensure_ascii=False, indent=2)
    pathlib.Path(str(file) + ".meta").write_text(text + "\n", encoding="utf-8", newline="\n")


def main():
    pictures = OUT / "картинки"
    sounds = OUT / "звуки"
    pictures.mkdir(parents=True, exist_ok=True)
    sounds.mkdir(parents=True, exist_ok=True)
    made = {
        "герой.png": hero(),
        "староста.png": villager((150, 60, 50, 255), (70, 60, 50, 255), hat=(110, 80, 40, 255), beard=True),
        "стражник.png": villager((90, 100, 120, 255), (60, 60, 70, 255), hat=(160, 165, 175, 255)),
        "пёс.png": dog(),
        "ключ.png": key(),
        "монеты.png": coins(),
        "сундук.png": chest(),
        "ворота.png": gate(),
    }
    for name, img in made.items():
        img.save(pictures / name, optimize=True)
        meta(pictures / name)
    ring = sounds / "звон.wav"
    shutil.copyfile(ROOT / "games" / "examples" / "screen-sound" / "sounds" / "звон.wav", ring)
    meta(ring)
    print(f"{len(made)} заглушек и звук записаны в {OUT}", file=sys.stderr)


if __name__ == "__main__":
    main()
