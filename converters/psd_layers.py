"""Photoshop (.psd, .psb) -> PNG: each layer on its own, or the whole picture."""

import json
import re
from pathlib import Path

from forge_convert import ConvertError


def safe_name(name, used):
    name = re.sub(r'[\\/:*?"<>|]+', "_", name).strip(" .") or "слой"
    out, n = name, 2
    while out.lower() in used:
        out = f"{name} {n}"
        n += 1
    used.add(out.lower())
    return out


def convert(source, out, settings, report):
    try:
        from psd_tools import PSDImage
    except ImportError as e:
        raise ImportError(name="psd_tools") from e
    what = settings.get("what", "layers")
    hidden = bool(settings.get("hidden", False))
    trim = bool(settings.get("trim", True))
    report(0.05, "читаю " + source.name)
    try:
        psd = PSDImage.open(source)
    except Exception as e:
        raise ConvertError(f"{source.name} не читается как файл Photoshop: {e}")
    files = []
    if what in ("flat", "both"):
        report(0.1, "собираю всю картинку")
        image = psd.composite(ignore_preview=True)
        if image is None:
            raise ConvertError(f"в {source.name} нечего рисовать")
        flat = out / (source.stem + ".png")
        image.save(flat)
        files.append(flat)
    if what in ("layers", "both"):
        layers = [l for l in psd.descendants() if not l.is_group() and (hidden or l.is_visible())]
        if not layers:
            raise ConvertError(f"в {source.name} нет {'' if hidden else 'видимых '}слоёв")
        folder = out / f"{source.stem} (слои)"
        folder.mkdir()
        used, index = set(), []
        for i, layer in enumerate(layers):
            report(0.15 + 0.8 * i / len(layers), f"слой «{layer.name}»")
            if trim:
                image = layer.composite(force=True)
                left, top = layer.left, layer.top
            else:
                image = layer.composite(viewport=psd.viewbox, force=True)
                left, top = 0, 0
            if image is None or image.width == 0:  # empty, or an adjustment layer
                continue
            # Groups become a prefix: "Герой - голова".
            path = []
            parent = layer.parent
            while parent is not None and parent is not psd:
                path.append(parent.name)
                parent = parent.parent
            name = safe_name(" - ".join(list(reversed(path)) + [layer.name]), used)
            file = folder / (name + ".png")
            image.save(file)
            index.append({"file": file.name, "name": layer.name, "x": left, "y": top,
                          "width": image.width, "height": image.height, "visible": layer.is_visible(),
                          "opacity": layer.opacity / 255})
        # Where each layer was, to put them back together in a game.
        (folder / "слои.json").write_text(json.dumps(
            {"width": psd.width, "height": psd.height, "layers": index}, ensure_ascii=False, indent=2),
            encoding="utf-8")
        files.append(folder)
    report(1, "готово")
    return files
