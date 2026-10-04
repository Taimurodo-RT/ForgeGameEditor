"""Tests of the converters: python -m unittest discover converters/tests"""

import json
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent


def run(converter, source, settings):
    """Runs a converter the way the editor does; returns (result, out folder, progress lines)."""
    tmp = Path(tempfile.mkdtemp())
    job = tmp / "job.json"
    out = tmp / "out"
    job.write_text(json.dumps({"converter": converter, "input": str(source), "output": str(out),
                               "settings": settings}, ensure_ascii=False), encoding="utf-8")
    p = subprocess.run([sys.executable, str(HERE / "forge_convert.py"), str(job)], capture_output=True)
    lines = [json.loads(l) for l in p.stdout.decode("utf-8").splitlines() if l.strip()]
    return lines[-1], out, lines[:-1]


def aseprite_file(path, frames, width=4, height=3):
    """A small RGBA .aseprite: one layer "тело", a hidden layer, a tag; frames: list of RGBA colours."""
    def string(s):
        b = s.encode("utf-8")
        return struct.pack("<H", len(b)) + b

    def chunk(kind, body):
        return struct.pack("<IH", 6 + len(body), kind) + body

    def layer(name, visible):
        return chunk(0x2004, struct.pack("<HHHHHHB", 1 if visible else 0, 0, 0, 0, 0, 0, 255) + b"\0" * 3 + string(name))

    body = b""
    for f, colour in enumerate(frames):
        chunks = []
        if f == 0:
            chunks += [layer("тело", True), layer("скрытый", False)]
            tag = struct.pack("<HHBH", 0, len(frames) - 1, 0, 0) + b"\0" * 10 + string("бег")
            chunks.append(chunk(0x2018, struct.pack("<H", 1) + b"\0" * 8 + tag))
        pixels = bytes(colour) * (2 * 2)
        cel = struct.pack("<HhhBHh", 0, 1, 1, 255, 2, 0) + b"\0" * 5 + struct.pack("<HH", 2, 2) + zlib.compress(pixels)
        chunks.append(chunk(0x2005, cel))
        hidden = bytes((255, 0, 0, 255)) * (width * height)
        chunks.append(chunk(0x2005, struct.pack("<HhhBHh", 1, 0, 0, 255, 0, 0) + b"\0" * 5 +
                            struct.pack("<HH", width, height) + hidden))
        data = b"".join(chunks)
        body += struct.pack("<IHHH", 16 + len(data), 0xF1FA, len(chunks), 100 + f) + b"\0\0" + struct.pack("<I", len(chunks)) + data
    header = struct.pack("<IHHHHHIH", 128 + len(body), 0xA5E0, len(frames), width, height, 32, 1, 100)
    header += b"\0" * (128 - len(header))
    path.write_bytes(header + body)


class Aseprite(unittest.TestCase):
    def test_sheet_in_a_row_with_frames_and_tags(self):
        from PIL import Image
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "герой.aseprite"
        aseprite_file(src, [(0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 0, 255)])
        result, out, progress = run("aseprite_sheet", src, {"layout": "row", "scale": "2"})
        self.assertTrue(result["ok"], result)
        self.assertTrue(progress and all(0 <= p["progress"] <= 1 for p in progress))
        sheet = Image.open(out / "герой.png")
        self.assertEqual(sheet.size, (3 * 8, 6))
        # Frame 2 is blue at its cel (1,1)-(3,3) scaled by 2; the hidden red layer is not drawn.
        self.assertEqual(sheet.getpixel((8 + 2, 2)), (0, 0, 255, 255))
        self.assertEqual(sheet.getpixel((8 + 0, 0))[3], 0)
        info = json.loads((out / "герой.json").read_text(encoding="utf-8"))
        self.assertEqual([f["duration"] for f in info["frames"]], [100, 101, 102])
        self.assertEqual(info["tags"][0]["name"], "бег")
        self.assertEqual(info["frames"][2]["x"], 16)

    def test_every_frame_its_own_file_and_hidden_layers(self):
        from PIL import Image
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "a.ase"
        aseprite_file(src, [(0, 255, 0, 255), (0, 0, 255, 255)])
        result, out, _ = run("aseprite_sheet", src, {"layout": "files", "hidden": True})
        self.assertTrue(result["ok"], result)
        folder = out / "a (кадры)"
        self.assertEqual(sorted(p.name for p in folder.iterdir()), ["001.png", "002.png", "кадры.json"])
        self.assertEqual(Image.open(folder / "001.png").getpixel((0, 0)), (255, 0, 0, 255))  # the hidden layer, shown

    def test_not_an_aseprite_file(self):
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "x.aseprite"
        src.write_bytes(b"hello")
        result, _, _ = run("aseprite_sheet", src, {})
        self.assertFalse(result["ok"])
        self.assertIn("не файл Aseprite", result["error"])


class Photoshop(unittest.TestCase):
    def make_psd(self, path):
        from PIL import Image
        from psd_tools import PSDImage
        from psd_tools.api.layers import PixelLayer
        psd = PSDImage.new("RGBA", (8, 6))
        back = PixelLayer.frompil(Image.new("RGBA", (8, 6), (255, 255, 255, 255)), psd, "back")
        star = PixelLayer.frompil(Image.new("RGBA", (2, 2), (255, 200, 0, 255)), psd, "star", top=2, left=3)
        ghost = PixelLayer.frompil(Image.new("RGBA", (1, 1), (0, 0, 0, 255)), psd, "ghost")
        ghost.visible = False
        psd.extend([back, star, ghost])
        psd.save(path)

    def test_layers_with_their_places(self):
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "сцена.psd"
        self.make_psd(src)
        result, out, _ = run("psd_layers", src, {"what": "both"})
        self.assertTrue(result["ok"], result)
        folder = out / "сцена (слои)"
        names = sorted(p.name for p in folder.iterdir())
        self.assertEqual(names, ["back.png", "star.png", "слои.json"])  # not the hidden one
        info = json.loads((folder / "слои.json").read_text(encoding="utf-8"))
        star = next(l for l in info["layers"] if l["name"] == "star")
        self.assertEqual((star["x"], star["y"], star["width"], star["height"]), (3, 2, 2, 2))
        self.assertTrue((out / "сцена.png").exists())

    def test_hidden_layers_and_full_size(self):
        from PIL import Image
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "s.psd"
        self.make_psd(src)
        result, out, _ = run("psd_layers", src, {"what": "layers", "hidden": True, "trim": False})
        self.assertTrue(result["ok"], result)
        folder = out / "s (слои)"
        self.assertTrue((folder / "ghost.png").exists())
        self.assertEqual(Image.open(folder / "star.png").size, (8, 6))


class Sound(unittest.TestCase):
    def tone(self, path, rate=44100, seconds=0.5, stereo=True, quiet=0.25):
        import numpy as np
        import soundfile as sf
        t = np.arange(int(rate * seconds)) / rate
        wave = quiet * np.sin(2 * np.pi * 440 * t)
        silence = np.zeros(int(rate * 0.2))
        wave = np.concatenate([silence, wave, silence])
        data = np.stack([wave, wave], axis=1) if stereo else wave
        sf.write(str(path), data, rate)

    def test_to_ogg_mono_lower_rate_even_loudness_trimmed(self):
        import numpy as np
        import soundfile as sf
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "шаг.wav"
        self.tone(src)
        result, out, _ = run("audio", src, {"format": "ogg", "channels": "mono", "rate": "22050",
                                             "normalize": True, "trim": True})
        self.assertTrue(result["ok"], result)
        data, rate = sf.read(str(out / "шаг.ogg"), always_2d=True)
        self.assertEqual((rate, data.shape[1]), (22050, 1))
        self.assertLess(len(data) / rate, 0.6)  # the silence is gone
        self.assertGreater(np.abs(data).max(), 0.8)  # louder than the 0.25 it was

    def test_as_it_is_to_wav(self):
        import soundfile as sf
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "a.flac"
        self.tone(src, rate=48000)
        result, out, _ = run("audio", src, {"format": "wav"})
        self.assertTrue(result["ok"], result)
        info = sf.info(str(out / "a.wav"))
        self.assertEqual((info.samplerate, info.channels), (48000, 2))

    def test_not_a_sound(self):
        tmp = Path(tempfile.mkdtemp())
        src = tmp / "x.mp3"
        src.write_bytes(b"not a sound")
        result, _, _ = run("audio", src, {})
        self.assertFalse(result["ok"])
        self.assertIn("не читается как звук", result["error"])


class Runner(unittest.TestCase):
    def test_every_manifest_is_valid(self):
        for manifest in HERE.glob("*.converter.json"):
            m = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertTrue(m["name"] and m["from"], manifest.name)
            self.assertTrue((HERE / (manifest.name.split(".")[0] + ".py")).exists(), manifest.name)
            for s in m.get("settings", []):
                self.assertIn(s["type"], ("choice", "bool", "number", "text"), s)
                if s["type"] == "choice":
                    self.assertIn(s["default"], [c["id"] for c in s["choices"]], s)

    def test_a_missing_library_is_said_plainly(self):
        tmp = Path(tempfile.mkdtemp())
        (tmp / "needs_lib.py").write_text("import no_such_library_here\n", encoding="utf-8")
        job = tmp / "job.json"
        job.write_text(json.dumps({"converter": "needs_lib", "folder": str(tmp), "input": "x", "output": str(tmp / "o")}))
        p = subprocess.run([sys.executable, str(HERE / "forge_convert.py"), str(job)], capture_output=True)
        result = json.loads(p.stdout.decode("utf-8").splitlines()[-1])
        self.assertFalse(result["ok"])
        self.assertIn("no_such_library_here", result["error"])


if __name__ == "__main__":
    unittest.main()
