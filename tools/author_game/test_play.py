"""play.py puts the source folders back: on a tree of its own (not the repository), a move that fails first, second or
third, a game that does not start, a game that fails, a folder that does not come back, and a run that goes well.

The folders are really moved, into the folder play.py makes and back. What is stubbed: the move that is to fail (it
raises before moving anything) and, but for the game that does not start, the game itself: subprocess.run returns the
exit code the case wants, and notes whether the folders were away while it "ran". So every case runs on Linux and on
Windows alike. The game that does not start is a real process start of a program that is not there.

    python3 -m unittest discover -s tools/author_game
"""

import hashlib
import importlib.util
import io
import shutil
import subprocess
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
        self.exe = self.base / "игра с пробелом"  # never started but in the case it does not start: then it is not there
        self.runs = []  # per stubbed start: were the three folders away then

    def play(self, exit_code=None, fail_away=None, fail_back=None, while_away=None):
        """play.py's main() with its ROOT and temp folder here. exit_code: what the stubbed game returns (None: the real
        start of self.exe, which is not there). The move number fail_away (out) or fail_back (back in) refuses;
        while_away() runs in the stubbed game. Its exit code and what it printed."""
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

        def run(args, **kwargs):
            self.runs.append([(self.root / "games" / name).exists() for name in FOLDERS])
            if while_away:
                while_away()
            return subprocess.CompletedProcess(args, exit_code)

        out = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        stubs = [mock.patch.object(shutil, "move", move), mock.patch.object(tempfile, "tempdir", str(self.tmp)),
                 mock.patch.object(sys, "argv", ["play.py", str(self.exe)])]
        if exit_code is not None:
            stubs.append(mock.patch.object(subprocess, "run", run))
        with redirect_stdout(out):
            for s in stubs:
                s.start()
            try:
                code = play.main()
            finally:
                for s in reversed(stubs):
                    s.stop()
        out.seek(0)
        return code, out.read()

    def aside(self) -> list:
        return sorted(q.name for d in self.root.glob("forge_sources_away_*") for q in d.iterdir())

    def assert_all_back(self):
        self.assertEqual(digests(self.root / "games"), self.before)
        self.assertEqual(list(self.root.glob("forge_sources_away_*")), [])

    def test_a_move_that_fails_first_second_or_third_puts_back_the_ones_moved(self):
        for n in (1, 2, 3):
            with self.subTest(move=n):
                code, said = self.play(exit_code=0, fail_away=n)
                self.assertEqual(code, 1)
                self.assertIn("прогон прерван: PermissionError", said)
                self.assertNotIn("НЕ ВОЗВРАЩЕНА", said)  # only the folders it moved are moved back
                self.assertEqual(self.runs, [])  # no game ran
                self.assert_all_back()

    def test_a_game_that_does_not_start_puts_them_back(self):
        code, said = self.play(exit_code=None)
        self.assertEqual(code, 1)
        self.assertIn("прогон прерван: FileNotFoundError", said)
        self.assert_all_back()

    def test_a_game_that_fails_puts_them_back(self):
        code, _ = self.play(exit_code=3)
        self.assertEqual(code, 1)
        self.assertEqual(self.runs, [[False, False, False]])  # it ran with the three away
        self.assert_all_back()

    def test_one_that_does_not_come_back_is_named_and_the_others_come_back(self):
        code, said = self.play(exit_code=0, fail_back=1)  # the last moved goes back first: modules
        self.assertEqual(code, 1)
        self.assertIn(f"НЕ ВОЗВРАЩЕНА {self.root / 'games' / 'modules'}", said)
        self.assertEqual(self.aside(), ["modules"])
        self.assertEqual(digests(self.root / "games"), {k: v for k, v in self.before.items() if not k.startswith("modules/")})

    def test_a_file_changed_while_away_fails_the_run(self):
        def change():
            for f in self.root.glob("forge_sources_away_*/platformer/sub/f1.json"):
                f.write_bytes(f.read_bytes() + b"x")
        code, said = self.play(exit_code=0, while_away=change)
        self.assertEqual(code, 1)
        self.assertIn("games/platformer: 3 файлов НЕ на месте или НЕ такие же", said)
        self.assertIn("games/slice: 3 файлов на месте с теми же SHA-256", said)

    def test_a_run_that_goes_well(self):
        code, said = self.play(exit_code=0)
        self.assertEqual(code, 0, said)
        self.assertEqual(self.runs, [[False, False, False]])
        self.assertIn("на месте с теми же SHA-256", said)
        self.assert_all_back()


if __name__ == "__main__":
    unittest.main()
