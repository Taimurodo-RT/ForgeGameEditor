"""The frame Tiled itself draws of games/examples/tiled, for the game to be compared with.

    python tools/tiled_example/rasterize.py      (Tiled 1.8.2: tiled and tmxrasterizer; with no screen it sets
                                                  QT_QPA_PLATFORM=offscreen)

writes games/examples/tiled/tmxrasterizer.png: the map as tmxrasterizer draws it, one pixel per pixel of the
tiles (--no-smoothing), the whole rectangle of its chunks (cells -16…48 × -16…32, 1024 × 768); and
tmxrasterizer_changed.png: the same after the author changed two pictures the map depends on, the chest's
(картинки/сундук.png, a picture of a tile object) and the decorations' tileset (наборы/декор.png, the picture of
an external .tsx), each with its colours turned over (255 - r, g, b; the see-through kept). What Forge does not
draw is left out of a copy of the map first, so what is left is what both draw:
- the background colour (Forge's sky is its own; the import says the colour is not carried over): the empty
  pixels stay see-through;
- the objects that are not pictures of tiles (the spawn point, the zone's rectangle, the ellipse, the polyline:
  Tiled draws them as shapes, Forge makes a point, a zone, nothing);
- the hidden group stays hidden (tmxrasterizer leaves out hidden layers, the import does not carry them over).
The object template is put into its objects (tiled --detach-templates), so the copy needs nothing but the map.
`forge_slice --test --scene tiled` draws the level the import made and compares it with the first frame pixel by
pixel where the frame is not see-through; `--scene tiled_update` changes the same two pictures in a copy of the map,
imports it again over the example and compares with the second.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLE = os.path.join(HERE, "..", "..", "games", "examples", "tiled")
SOURCE = os.path.join(EXAMPLE, "Карта Tiled")
CHANGED = ["картинки/сундук.png", "наборы/декор.png"]  # as forge_slice --scene tiled_update changes them


def turn_over(path):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    px = im.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b, a = px[x, y]
            px[x, y] = (255 - r, 255 - g, 255 - b, a)
    im.save(path)


def rasterize(out, changed, env):
    with tempfile.TemporaryDirectory() as tmp:
        # A copy of the whole folder: the map's tilesets, pictures and template are found next to it.
        folder = os.path.join(tmp, "map")
        shutil.copytree(SOURCE, folder)
        if changed:
            for name in CHANGED:
                turn_over(os.path.join(folder, name))
        flat = os.path.join(folder, "flat.tmx")
        subprocess.run(["tiled", "--export-map", "tmx", os.path.join(folder, "уровень.tmx"), flat, "--detach-templates"],
                       check=True, env=env)
        tree = ET.parse(flat)
        root = tree.getroot()
        root.attrib.pop("backgroundcolor", None)
        gone = 0
        for group in root.iter("objectgroup"):
            for o in list(group.findall("object")):
                if "gid" not in o.attrib:
                    group.remove(o)
                    gone += 1
        tree.write(flat, encoding="UTF-8", xml_declaration=True)
        png = os.path.join(tmp, "frame.png")
        subprocess.run(["tmxrasterizer", "--no-smoothing", flat, png], check=True, env=env)
        shutil.copyfile(png, out)
    print(f"{os.path.normpath(out)}: left out {gone} objects that are not pictures of tiles")


def main():
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    rasterize(os.path.join(EXAMPLE, "tmxrasterizer.png"), False, env)
    rasterize(os.path.join(EXAMPLE, "tmxrasterizer_changed.png"), True, env)


if __name__ == "__main__":
    sys.exit(main())
