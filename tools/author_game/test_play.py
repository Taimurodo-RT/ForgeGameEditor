"""play.py puts the source folders back: on a tree of its own (not the repository), a move that fails second or third, a
game that does not start or fails, a folder that does not come back, and a run that goes well.

    python3 -m unittest discover -s tools/author_game
"""

import hashlib
import importlib.util
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

PLAY = Path(__file__).resolve().parent / "play.py"
FOLDERS = ("slice", "platformer", "modules")


def digests(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob("*")) if p.is_file()}


class Staging(unittest.TestCase):
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix="forge_play_test_"))
        self.addCleanup(shutil.rmtree, self.base, True)
        self.root, self.tmp = self.base / "repo", self.base / "tmp"
        for name in FOLDERS:
            for i in range(3):
                f = self.root / "games" / name / "sub" / f"f{i}.json"
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_bytes(f"{name} {i}\r\n".encode() * (i + 1))
        game = self.tmp / "forge_editor_новые игры" / "Мои игры" / "Игра А"
        (game / "game").mkdir(parents=True)
        (game / "project.forge").write_text("{}", encoding="utf-8")
        self.before = digests(self.root / "games")

    def exe(self, code: int) -> Path:
        exe = self.base / "игра с пробелом"
        exe.write_text(f"#!/bin/sh\nexit {code}\n", encoding="utf-8")
        exe.chmod(0o755)
        return exe

    def play(self, exe: Path, fail_away=None, fail_back=None):
        """play.py's main() with its ROOT and temp folder here; the move number fail_away (out) or fail_back (back in)
        refuses. Its exit code and what it printed."""
        spec = importlib.util.spec_from_file_location("play_under_test", PLAY)
        play = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(play)
        play.ROOT = self.root
        real_move, calls = shutil.move, {"away": 0, "back": 0}

        def move(src, dst):
            key = "back" if Path(src).parent.name.startswith("forge_sources_away_") else "away"
            calls[key] += 1
            if calls[key] == {"away": fail_away, "back": fail_back}[key]:
                raise PermissionError(f"{src}: занята")
            return real_move(src, dst)

        out = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        with mock.patch.object(shutil, "move", move), mock.patch.object(tempfile, "tempdir", str(self.tmp)), \
             mock.patch.object(sys, "argv", ["play.py", str(exe)]), redirect_stdout(out):
            code = play.main()
        out.seek(0)
        return code, out.read()

    def aside(self) -> list:
        return sorted(q.name for d in self.root.glob("forge_sources_away_*") for q in d.iterdir())

    def test_a_move_that_fails_second_or_third_puts_back_the_ones_moved(self):
        for n in (1, 2, 3):
            with self.subTest(move=n):
                code, said = self.play(self.base / "нет игры", fail_away=n)
                self.assertEqual(code, 1)
                self.assertIn("прогон прерван: PermissionError", said)
                self.assertEqual(digests(self.root / "games"), self.before)
                self.assertEqual(self.aside(), [])

    def test_a_game_that_does_not_start_puts_them_back(self):
        code, said = self.play(self.base / "нет игры")
        self.assertEqual(code, 1)
        self.assertIn("прогон прерван", said)
        self.assertEqual(digests(self.root / "games"), self.before)
        self.assertEqual(self.aside(), [])

    @unittest.skipIf(os.name == "nt", "a shell script for the game")
    def test_a_game_that_fails_puts_them_back(self):
        code, _ = self.play(self.exe(3))
        self.assertEqual(code, 1)
        self.assertEqual(digests(self.root / "games"), self.before)
        self.assertEqual(self.aside(), [])

    @unittest.skipIf(os.name == "nt", "a shell script for the game")
    def test_one_that_does_not_come_back_is_named_and_the_others_come_back(self):
        code, said = self.play(self.exe(0), fail_back=1)  # moved back last first: modules
        self.assertEqual(code, 1)
        self.assertIn(f"НЕ ВОЗВРАЩЕНА {self.root / 'games' / 'modules'}", said)
        self.assertEqual(self.aside(), ["modules"])
        after = digests(self.root / "games")
        self.assertEqual(after, {k: v for k, v in self.before.items() if not k.startswith("modules/")})

    @unittest.skipIf(os.name == "nt", "a shell script for the game")
    def test_a_run_that_goes_well(self):
        code, said = self.play(self.exe(0))
        self.assertEqual(code, 0, said)
        self.assertEqual(digests(self.root / "games"), self.before)
        self.assertEqual(list(self.root.glob("forge_sources_away_*")), [])


if __name__ == "__main__":
    unittest.main()
