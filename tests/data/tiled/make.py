"""Maps for tests/test_tiled.cpp, written by Tiled itself.

The example's map (games/examples/tiled/Карта Tiled/уровень.tmx) with its layer data in other encodings, a finite
map, maps Forge does not take and broken ones. Each good one is written here as text and then re-saved by Tiled
(1.8.2, `tiled --export-map`), so the test reads what Tiled writes:
    QT_QPA_PLATFORM=offscreen python tests/data/tiled/make.py
The broken ones (which Tiled would not save) are written by hand and say so in their first comment.
"""
import base64
import gzip
import os
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "..", "games", "examples", "tiled", "Карта Tiled")
REL = "../../../games/examples/tiled/Карта Tiled/"


def example():
    tree = ET.parse(os.path.join(SRC, "уровень.tmx"))
    root = tree.getroot()
    # Paths from here.
    for ts in root.iter("tileset"):
        if "source" in ts.attrib:
            ts.set("source", REL + ts.get("source"))
    for img in root.iter("image"):
        img.set("source", REL + img.get("source"))
    for o in root.iter("object"):
        if "template" in o.attrib:
            o.set("template", REL + o.get("template"))
    for p in root.iter("property"):
        if p.get("type") == "file":
            p.set("value", REL + p.get("value"))
    return tree


def gids_of(holder):
    return [int(v) for v in holder.text.replace("\n", "").split(",") if v.strip()]


def encode(tree, encoding, compression=None):
    for data in tree.getroot().iter("data"):
        holders = data.findall("chunk") or [data]
        for h in holders:
            gids = gids_of(h)
            h.text = None
            for c in list(h):
                h.remove(c)
            raw = b"".join(struct.pack("<I", g) for g in gids)
            if encoding == "xml":
                for g in gids:
                    t = ET.SubElement(h, "tile")
                    if g:
                        t.set("gid", str(g))
                continue
            if compression == "zlib":
                raw = zlib.compress(raw)
            elif compression == "gzip":
                raw = gzip.compress(raw)
            h.text = base64.b64encode(raw).decode()
        data.attrib.pop("compression", None)
        if encoding == "xml":
            data.attrib.pop("encoding", None)
        else:
            data.set("encoding", "base64")
            if compression:
                data.set("compression", compression)
    return tree


def write(tree, name):
    path = os.path.join(HERE, name)
    tree.write(path, encoding="UTF-8", xml_declaration=True)
    return path


def tiled(path):
    env = dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"))
    subprocess.run(["tiled", "--export-map", "tmx", path, path], check=True, env=env)


FINITE = '''<?xml version="1.0" encoding="UTF-8"?>
<map version="1.8" orientation="orthogonal" renderorder="right-down" width="4" height="3" tilewidth="16" tileheight="16" infinite="0" nextlayerid="2" nextobjectid="1">
 <tileset firstgid="1" source="''' + REL + '''наборы/декор.tsx"/>
 <layer id="1" name="Слой" width="4" height="3" offsetx="32" offsety="-16">
  <data encoding="csv">
1,0,0,2,
0,3,0,0,
0,0,0,2147483652
</data>
 </layer>
</map>
'''

HEX = '''<?xml version="1.0" encoding="UTF-8"?>
<map version="1.8" orientation="hexagonal" renderorder="right-down" width="2" height="2" tilewidth="16" tileheight="16" infinite="0" hexsidelength="8" staggeraxis="y" staggerindex="odd" nextlayerid="2" nextobjectid="1">
 <tileset firstgid="1" source="''' + REL + '''наборы/декор.tsx"/>
 <layer id="1" name="Слой" width="2" height="2">
  <data encoding="csv">
1,0,
0,1
</data>
 </layer>
</map>
'''

NOT_SQUARE = FINITE.replace('tileheight="16" infinite', 'tileheight="8" infinite')

BROKEN = {
    "zstd.tmx": FINITE.replace('<data encoding="csv">\n1,0,0,2,\n0,3,0,0,\n0,0,0,2147483652\n</data>',
                               '<data encoding="base64" compression="zstd">KLUv/SAwAQAA</data>'),
    "битый base64.tmx": FINITE.replace('<data encoding="csv">\n1,0,0,2,\n0,3,0,0,\n0,0,0,2147483652\n</data>',
                                      '<data encoding="base64">AQAA*AAA</data>'),
    "не XML.tmx": "Это не карта: Tiled её не писал.\n",
    "набор JSON.tmx": FINITE.replace(REL + "наборы/декор.tsx", "декор.tsj"),
    "номер без набора.tmx": FINITE.replace('firstgid="1"', 'firstgid="5"'),
    "мало клеток.tmx": FINITE.replace("0,0,0,2147483652\n", "0,0,0\n"),
}


def as_tiled_reads(name, out):
    """What Tiled itself reads from a map (its JSON export, templates detached), as lines of text for the test:
    "layer <name>|<visible>|<chunk x> <y> <w> <h>|<gid> <gid> …" and "object <id>|<name>|<shape>|<x> <y> <w> <h>|<gid>"."""
    env = dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"))
    tmp = os.path.join(HERE, "_tiled.json")
    subprocess.run(["tiled", "--export-map", "--detach-templates", "json", name, tmp], check=True, env=env)
    import json
    with open(tmp, encoding="utf-8") as f:
        m = json.load(f)
    os.remove(tmp)
    lines = []

    def walk(layers, path, visible):
        for l in layers:
            v = visible and l.get("visible", True)
            if l["type"] == "group":
                walk(l["layers"], path + l["name"] + "/", v)
            elif l["type"] == "tilelayer":
                parts = l.get("chunks") or [l]
                for c in parts:
                    lines.append(f'layer {path}{l["name"]}|{int(v)}|{c.get("x", 0)} {c.get("y", 0)} {c["width"]} {c["height"]}|'
                                 + " ".join(str(g) for g in c["data"]))
            elif l["type"] == "objectgroup":
                for o in l["objects"]:
                    shape = next((k for k in ("ellipse", "point", "polygon", "polyline", "text", "capsule") if k in o), "rect")
                    lines.append(f'object {o["id"]}|{o.get("name", "")}|{shape}|{o["x"]:g} {o["y"]:g} {o["width"]:g} {o["height"]:g}|'
                                 f'{o.get("gid", 0)}')
    walk(m["layers"], "", True)
    with open(os.path.join(HERE, out), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    for name, enc, comp in [("формат base64.tmx", "base64", None), ("формат zlib.tmx", "base64", "zlib"),
                            ("формат gzip.tmx", "base64", "gzip"), ("формат XML.tmx", "xml", None)]:
        tiled(write(encode(example(), enc, comp), name))
    for name, text in [("конечная.tmx", FINITE), ("гексы.tmx", HEX), ("клетки 16x8.tmx", NOT_SQUARE)]:
        path = os.path.join(HERE, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        tiled(path)
    for name, text in BROKEN.items():
        with open(os.path.join(HERE, name), "w", encoding="utf-8", newline="\n") as f:
            if text.startswith("<?xml"):
                text = text.replace("?>\n", "?>\n<!-- Написан вручную: Tiled такой файл не сохранит. -->\n", 1)
            f.write(text)
    as_tiled_reads(os.path.join(SRC, "уровень.tmx"), "уровень как его читает Tiled.txt")
    print("written:", HERE)


if __name__ == "__main__":
    sys.exit(main())
