"""The Tiled map of games/examples/tiled, its tilesets and object template, as text.

The files are then re-saved by Tiled itself, so the example is what Tiled writes:
    python tools/tiled_example/make_art.py
    python tools/tiled_example/make_map.py
    cd "games/examples/tiled/Карта Tiled"
    tiled --export-tileset tsx "наборы/Земля и камень.tsx" "наборы/Земля и камень.tsx"   (and the other two)
    tiled --export-map tmx уровень.tmx уровень.tmx
(Tiled 1.8.2; with no screen: QT_QPA_PLATFORM=offscreen.) Tiled renumbers firstgids and counts the tiles of an
atlas from its picture, so the numbers below are only where the text starts.

What the map has, to show every way a Tiled map comes into Forge:
- an infinite map: chunks of 16 × 16 cells, some at negative x and y (a ledge and a cloud left of and above 0, 0);
- four tilesets: an atlas with a margin and spacing in an external .tsx whose path has Cyrillic letters and a
  space ("наборы/Земля и камень.tsx"), one in the map with a see-through colour (#ff00ff), one more external
  .tsx, and a collection of images with ids that skip numbers and a picture twice as tall;
- tiles turned all eight ways (the ore under the grass), mirrored clouds and arrows;
- layers: the sky, the background, decorations in a group (over the background: they stack), the ground with
  the bool property solid, and a hidden layer in a hidden group (not carried over);
- objects: the hero's spawn point, a rectangle with the music property (a zone), pictures of tiles (one mirrored,
  one stretched to 32 × 32), two lanterns from the object template "шаблоны/фонарь.tx" (one with a name of its
  own, at negative x), and an ellipse and a polyline, which Forge does not carry over (the import says so).
"""
import os
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "games", "examples", "tiled", "Карта Tiled")
FH, FV, FD = 0x80000000, 0x40000000, 0x20000000
G = 1       # Земля и камень (13 tiles)
SKY = 14    # Небо (4)
DEC = 18    # декор (4)
THING = 22  # предметы (ids 0, 3, 7)
X0, Y0, X1, Y1 = -16, -16, 48, 32  # the chunks written (multiples of 16)
GROUND_NAMES = ["Трава", "Земля", "Камень", "Тёмный камень", "Кирпич", "Доски", "Угол травы", "Руда",
                "Стена пещеры", "Кирпичная стена", "Окно", "Стрелка", "Лестница"]
SOLID = set(range(0, 8))


def grid():
    return {}


sky, back, decor, ground, hidden = grid(), grid(), grid(), grid(), grid()

# Sky: clouds and the sun; one cloud above y 0 and left of x 0 (a chunk at negative x and y).
sky[(5, 2)] = SKY + 0
sky[(6, 2)] = SKY + 1
sky[(15, 3)] = SKY + 0
sky[(16, 3)] = SKY + 1
sky[(30, 1)] = SKY + 2
sky[(22, 4)] = (SKY + 1) | FH  # a cloud mirrored
sky[(-7, -3)] = SKY + 0
sky[(-6, -3)] = SKY + 1

# Ground: grass on top (row 14), dirt below, stone deeper, from x -12 to 39.
for x in range(-12, 40):
    ground[(x, 14)] = G + 0
    ground[(x, 15)] = G + 1
    ground[(x, 16)] = G + 1
    ground[(x, 17)] = G + 2
ground[(-12, 14)] = (G + 6)  # the grass corner at the left edge
# The ore in all eight turns, in a row under the grass.
for i, f in enumerate([0, FH, FV, FH | FV, FD, FD | FH, FD | FV, FD | FH | FV]):
    ground[(2 + i, 16)] = (G + 7) | f
# A ledge of planks over the left part (negative x).
for x in range(-9, -4):
    ground[(x, 10)] = G + 5
# A brick house x 6..12, its wall behind.
for y in range(9, 14):
    for x in range(6, 13):
        back[(x, y)] = G + 9
back[(9, 11)] = G + 10  # the window
for x in range(6, 13):
    ground[(x, 8)] = G + 4  # the roof
# A wooden platform with grass corners turned: x 14..17, y 11.
ground[(14, 11)] = G + 6
ground[(15, 11)] = G + 5
ground[(16, 11)] = G + 5
ground[(17, 11)] = (G + 6) | FH
ground[(16, 10)] = (G + 6) | FD
ground[(15, 10)] = (G + 6) | FV | FH
# The cave x 24..37, y 8..13: stone around, its wall behind, an opening on the left (y 11..13).
for x in range(24, 38):
    ground[(x, 8)] = G + 2
for y in range(8, 14):
    ground[(37, y)] = G + 3
    if y < 11:
        ground[(24, y)] = G + 3
for y in range(9, 14):
    for x in range(25, 37):
        back[(x, y)] = G + 8
back[(26, 12)] = G + 11          # an arrow pointing in
back[(35, 12)] = (G + 11) | FH   # and one pointing back out
for y in range(9, 14):
    back[(30, y)] = G + 12        # a ladder
# Decor: flowers and a mushroom on the grass (one left of x 0), vines and moss over the cave wall (stacked on it).
decor[(2, 13)] = DEC + 0
decor[(4, 13)] = DEC + 0
decor[(-4, 13)] = DEC + 1
decor[(19, 13)] = DEC + 1
decor[(20, 13)] = (DEC + 0) | FH
for y in range(9, 12):
    decor[(28, y)] = DEC + 2
    decor[(33, y)] = (DEC + 2) | FH
for x in range(25, 30):
    decor[(x, 9)] = DEC + 3
# A hidden layer: a note to the author, not part of the level.
hidden[(10, 5)] = G + 11


def chunks(cells):
    """The layer's data: csv chunks of 16 × 16, only those with something in them."""
    out = ""
    for cy in range(Y0, Y1, 16):
        for cx in range(X0, X1, 16):
            if not any(cx <= x < cx + 16 and cy <= y < cy + 16 for (x, y) in cells):
                continue
            rows = [",".join(str(cells.get((cx + i, cy + j), 0)) for i in range(16)) for j in range(16)]
            out += f'   <chunk x="{cx}" y="{cy}" width="16" height="16">\n' + ",\n".join(rows) + "\n</chunk>\n"
    return out


def layer(i, name, cells, extra="", props=""):
    return (f'  <layer id="{i}" name="{name}" width="{X1 - X0}" height="{Y1 - Y0}"{extra}>\n{props}'
            f'   <data encoding="csv">\n{chunks(cells)}   </data>\n  </layer>\n')


SOLID_PROPS = '   <properties>\n    <property name="solid" type="bool" value="true"/>\n   </properties>\n'
TMX = f'''<?xml version="1.0" encoding="UTF-8"?>
<map version="1.8" tiledversion="1.8.2" orientation="orthogonal" renderorder="right-down" width="40" height="18" tilewidth="16" tileheight="16" infinite="1" backgroundcolor="#78aee6" nextlayerid="10" nextobjectid="11">
 <tileset firstgid="{G}" source="наборы/Земля и камень.tsx"/>
 <tileset firstgid="{SKY}" name="Небо" tilewidth="16" tileheight="16" tilecount="4" columns="4">
  <image source="небо.png" trans="ff00ff" width="64" height="16"/>
 </tileset>
 <tileset firstgid="{DEC}" source="наборы/декор.tsx"/>
 <tileset firstgid="{THING}" source="наборы/предметы.tsx"/>
{layer(1, "Небо", sky)}{layer(2, "Фон", back)} <group id="3" name="Украшения">
{layer(4, "Декор", decor)} </group>
{layer(5, "Земля", ground, "", SOLID_PROPS)} <group id="6" name="Подсказки" visible="0">
{layer(7, "Скрытое", hidden)} </group>
 <objectgroup id="8" name="Объекты">
  <object id="1" name="spawn" x="48" y="224">
   <point/>
  </object>
  <object id="2" name="Пещера" x="400" y="144" width="192" height="80">
   <properties>
    <property name="music" type="file" value="музыка/пещера.wav"/>
   </properties>
  </object>
  <object id="3" name="Сундук" gid="{THING + 0}" x="544" y="224" width="16" height="16"/>
  <object id="4" name="Табличка" gid="{THING + 3}" x="288" y="224" width="16" height="16"/>
  <object id="5" name="Табличка назад" gid="{(THING + 3) | FH}" x="368" y="224" width="16" height="16"/>
  <object id="6" template="шаблоны/фонарь.tx" x="336" y="224"/>
  <object id="7" name="Большой сундук" gid="{THING + 0}" x="160" y="224" width="32" height="32"/>
  <object id="8" name="Фонарь у обрыва" template="шаблоны/фонарь.tx" x="-176" y="224"/>
  <object id="9" name="Облако" x="96" y="-24" width="48" height="24">
   <ellipse/>
  </object>
  <object id="10" name="Тропа" x="64" y="200">
   <polyline points="0,0 64,-16 128,0"/>
  </object>
 </objectgroup>
</map>
'''


def tsx_ground():
    shape = ('\n  <objectgroup draworder="index" id="2">\n   <object id="1" x="0" y="0" width="16" height="16"/>\n'
             '  </objectgroup>\n ')
    tiles = "".join(f' <tile id="{i}" type="{n}">{shape if i in SOLID else ""}</tile>\n' for i, n in enumerate(GROUND_NAMES))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<tileset version="1.8" tiledversion="1.8.2" name="Земля и камень" '
            'tilewidth="16" tileheight="16" spacing="2" margin="1" tilecount="13" columns="8">\n'
            ' <image source="Земля и камень.png" width="144" height="36"/>\n' + tiles + '</tileset>\n')


def tsx_decor():
    names = ["Цветок", "Мухомор", "Лоза", "Мох"]
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<tileset version="1.8" tiledversion="1.8.2" name="Декор" '
            'tilewidth="16" tileheight="16" tilecount="4" columns="4">\n <image source="декор.png" width="64" height="16"/>\n'
            + "".join(f' <tile id="{i}" type="{n}"/>\n' for i, n in enumerate(names)) + '</tileset>\n')


TSX_THINGS = '''<?xml version="1.0" encoding="UTF-8"?>
<tileset version="1.8" tiledversion="1.8.2" name="Предметы" tilewidth="16" tileheight="32" tilecount="3" columns="0">
 <grid orientation="orthogonal" width="1" height="1"/>
 <tile id="0" type="Сундук">
  <image width="16" height="16" source="../картинки/сундук.png"/>
 </tile>
 <tile id="3" type="Табличка">
  <image width="16" height="16" source="../картинки/табличка.png"/>
 </tile>
 <tile id="7" type="Фонарь">
  <image width="16" height="32" source="../картинки/фонарь.png"/>
 </tile>
</tileset>
'''

TX_LAMP = '''<?xml version="1.0" encoding="UTF-8"?>
<template>
 <tileset firstgid="1" source="../наборы/предметы.tsx"/>
 <object name="Фонарь" gid="8" width="16" height="32"/>
</template>
'''


def main():
    for d in ("наборы", "шаблоны"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    files = {
        "уровень.tmx": TMX,
        os.path.join("наборы", "Земля и камень.tsx"): tsx_ground(),
        os.path.join("наборы", "декор.tsx"): tsx_decor(),
        os.path.join("наборы", "предметы.tsx"): TSX_THINGS,
        os.path.join("шаблоны", "фонарь.tx"): TX_LAMP,
    }
    for name, text in files.items():
        with open(os.path.join(ROOT, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print("written:", ROOT)


if __name__ == "__main__":
    sys.exit(main())
