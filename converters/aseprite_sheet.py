"""Aseprite (.aseprite, .ase) -> a sprite sheet PNG with its frames and tags.

The file format is read here (no Aseprite needed): layers, cels (raw,
linked, compressed), palettes, tags, frame durations; RGBA, grayscale and
indexed pictures. Layers are drawn bottom to top with their opacity
("normal" blending; other modes are drawn as normal too).
"""

import json
import struct
import zlib
from pathlib import Path

from forge_convert import ConvertError


class Reader:
    def __init__(self, data, at=0):
        self.data, self.at = data, at

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.data, self.at)
        self.at += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]

    def bytes(self, n):
        v = self.data[self.at:self.at + n]
        self.at += n
        return v

    def string(self):
        n = self.take("H")
        return self.bytes(n).decode("utf-8", "replace")


def read(source):
    data = source.read_bytes()
    if len(data) < 128 or struct.unpack_from("<H", data, 4)[0] != 0xA5E0:
        raise ConvertError(f"{source.name} — не файл Aseprite")
    r = Reader(data)
    _size, _magic, frames, width, height, depth, _flags = r.take("IHHHHHI")
    r.at = 28
    transparent = r.take("B")
    r.at = 128
    doc = {"width": width, "height": height, "depth": depth, "transparent": transparent,
           "layer_opacity": bool(_flags & 1),
           "layers": [], "frames": [], "tags": [], "palette": [(0, 0, 0, 255)] * 256}
    for f in range(frames):
        start = r.at
        frame_size, magic, old_chunks, duration = r.take("IHHH")
        if magic != 0xF1FA:
            raise ConvertError(f"{source.name}: кадр {f + 1} повреждён")
        r.at += 2
        new_chunks = r.take("I")
        chunks = new_chunks or old_chunks
        frame = {"duration": duration, "cels": []}
        for _ in range(chunks):
            c_start = r.at
            c_size, c_type = r.take("IH")
            body = Reader(data, r.at)
            if c_type == 0x2004:  # layer
                flags, kind, level, _w, _h, blend, opacity = body.take("HHHHHHB")
                body.at += 3
                name = body.string()
                doc["layers"].append({"name": name, "visible": bool(flags & 1), "group": kind == 1,
                                      "level": level, "opacity": opacity, "blend": blend,
                                      "tilemap": kind == 2})
            elif c_type == 0x2005:  # cel
                layer, x, y, opacity, kind, _z = body.take("HhhBHh")
                body.at += 5
                cel = {"layer": layer, "x": x, "y": y, "opacity": opacity}
                if kind == 1:
                    cel["link"] = body.take("H")
                elif kind in (0, 2):
                    w, h = body.take("HH")
                    raw = body.bytes(c_start + c_size - body.at)
                    cel.update(w=w, h=h, pixels=zlib.decompress(raw) if kind == 2 else raw)
                else:
                    continue  # tilemaps: not drawn
                frame["cels"].append(cel)
            elif c_type == 0x2019:  # palette
                _n, first, last = body.take("III")
                body.at += 8
                for i in range(first, last + 1):
                    eflags, rr, gg, bb, aa = body.take("HBBBB")
                    if eflags & 1:
                        body.string()
                    if i < 256:
                        doc["palette"][i] = (rr, gg, bb, aa)
            elif c_type in (0x0004, 0x0011):  # old palettes
                packets = body.take("H")
                i = 0
                for _ in range(packets):
                    i += body.take("B")
                    n = body.take("B") or 256
                    for _ in range(n):
                        rr, gg, bb = body.take("BBB")
                        if c_type == 0x0011:
                            rr, gg, bb = (rr << 2) | (rr >> 4), (gg << 2) | (gg >> 4), (bb << 2) | (bb >> 4)
                        if i < 256:
                            doc["palette"][i] = (rr, gg, bb, 255)
                        i += 1
            elif c_type == 0x2018:  # tags
                n = body.take("H")
                body.at += 8
                for _ in range(n):
                    a, b, direction, _repeat = body.take("HHBH")
                    body.at += 10
                    doc["tags"].append({"name": body.string(), "from": a, "to": b,
                                        "direction": ["forward", "reverse", "pingpong", "pingpong_reverse"][min(direction, 3)]})
            r.at = c_start + c_size
        doc["frames"].append(frame)
        r.at = start + frame_size
    return doc


def cel_image(doc, cel):
    from PIL import Image
    w, h, px = cel["w"], cel["h"], cel["pixels"]
    if doc["depth"] == 32:
        return Image.frombytes("RGBA", (w, h), px[:w * h * 4])
    if doc["depth"] == 16:
        return Image.frombytes("LA", (w, h), px[:w * h * 2]).convert("RGBA")
    pal = doc["palette"]
    t = doc["transparent"]
    out = bytearray()
    for i in px[:w * h]:
        out += bytes((0, 0, 0, 0)) if i == t else bytes(pal[i])
    return Image.frombytes("RGBA", (w, h), bytes(out))


def visible_layers(doc, hidden):
    """Layer indexes to draw: visible ones whose groups are visible too."""
    out, parents = [], []
    for i, layer in enumerate(doc["layers"]):
        del parents[layer["level"]:]
        shown = hidden or (layer["visible"] and all(p["visible"] for p in parents))
        if layer["group"]:
            parents.append(layer)
        elif shown and not layer["tilemap"]:
            out.append(i)
    return out


def draw_frame(doc, f, layers):
    from PIL import Image
    canvas = Image.new("RGBA", (doc["width"], doc["height"]), (0, 0, 0, 0))
    cels = {c["layer"]: c for c in doc["frames"][f]["cels"]}
    for i in layers:
        cel = cels.get(i)
        if cel is None:
            continue
        if "link" in cel:
            cel = next((c for c in doc["frames"][cel["link"]]["cels"] if c["layer"] == i and "pixels" in c), None)
            if cel is None:
                continue
        image = cel_image(doc, cel)
        layer_opacity = doc["layers"][i]["opacity"] if doc["layer_opacity"] else 255
        alpha = cel["opacity"] * layer_opacity / (255 * 255)
        if alpha < 1:
            a = image.getchannel("A").point(lambda v: int(v * alpha))
            image.putalpha(a)
        layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        layer.paste(image, (cel["x"], cel["y"]))
        canvas = Image.alpha_composite(canvas, layer)
    return canvas


def convert(source, out, settings, report):
    from PIL import Image
    report(0.05, "читаю " + source.name)
    try:
        doc = read(source)
    except ConvertError:
        raise
    except Exception as e:
        raise ConvertError(f"{source.name} не читается: {e}")
    layout = settings.get("layout", "row")
    scale = max(1, int(settings.get("scale", 1)))
    hidden = bool(settings.get("hidden", False))
    split = settings.get("layers", "merged") == "each"
    count = len(doc["frames"])
    if count == 0:
        raise ConvertError(f"в {source.name} нет кадров")
    w, h = doc["width"] * scale, doc["height"] * scale
    groups = [(source.stem, visible_layers(doc, hidden))]
    if split:
        groups = [(f"{source.stem} - {doc['layers'][i]['name']}", [i]) for i in visible_layers(doc, hidden)]
    files = []
    for g, (name, layers) in enumerate(groups):
        frames = []
        for f in range(count):
            report(0.1 + 0.85 * (g * count + f) / (len(groups) * count), f"кадр {f + 1} из {count}")
            image = draw_frame(doc, f, layers)
            if scale > 1:
                image = image.resize((w, h), Image.NEAREST)
            frames.append(image)
        if layout == "files":
            folder = out / f"{name} (кадры)"
            folder.mkdir()
            for f, image in enumerate(frames):
                image.save(folder / f"{f + 1:03d}.png")
            info_file = folder / "кадры.json"
            rects = [{"file": f"{f + 1:03d}.png", "duration": doc["frames"][f]["duration"]} for f in range(count)]
            files.append(folder)
        else:
            columns = count if layout == "row" else max(1, int(round(count ** 0.5 + 0.4999)))
            rows = (count + columns - 1) // columns
            sheet = Image.new("RGBA", (columns * w, rows * h), (0, 0, 0, 0))
            rects = []
            for f, image in enumerate(frames):
                x, y = (f % columns) * w, (f // columns) * h
                sheet.paste(image, (x, y))
                rects.append({"x": x, "y": y, "w": w, "h": h, "duration": doc["frames"][f]["duration"]})
            sheet_file = out / (name + ".png")
            sheet.save(sheet_file)
            files.append(sheet_file)
            info_file = out / (name + ".json")
            files.append(info_file)
        info = {"source": source.name, "frame_width": w, "frame_height": h, "frames": rects, "tags": doc["tags"]}
        info_file.write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding="utf-8")
    report(1, "готово")
    return files
