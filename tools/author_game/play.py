"""Plays the author's game «Игра А» that `forge_editor --self-test new-game` made
of the template «Старая шахта», in each game given, with games/slice moved away
for the run: the game must take everything of it from the game's own folder.

    python3 tools/author_game/play.py build/dist/OldMine/OldMine build/apps/slice/forge_slice

Each game runs `--test --scene project --play --data <Игра А>/game --level
<Игра А>/game/level --user <a folder of its own>` and must exit with 0.

Then, when `forge_editor --self-test two-games` has made them (step 14.1b), its
«Игра А» and «Игра Б» the same way, each with `--edits` of what the editor
wrote it must have: A's «Находка» (its picture on the level, the value, the
window, the button's sound), none of it in B. This plays a game's own folder
with a game program (OldMine of the template, or the build's); it is not an
export of the author's game.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # Cyrillic paths in a Windows CI log
    games = [Path(p).resolve() for p in sys.argv[1:]]
    if not games:
        print(__doc__)
        return 2
    game = Path(tempfile.gettempdir()) / "forge_editor_новые игры" / "Мои игры" / "Игра А"
    if not (game / "project.forge").is_file():
        print(f"нет игры {game}: сначала forge_editor --self-test new-game", flush=True)
        return 1
    # The games of two-games, each with what it must (not) have.
    two = Path(tempfile.gettempdir()) / "forge_editor_две игры"
    plays = [(game, None)]
    if (two / "ожидания А.json").is_file() and (two / "ожидания Б.json").is_file():
        plays += [(two / "Мои игры" / "Игра А", two / "ожидания А.json"), (two / "Мои игры" / "Игра Б", two / "ожидания Б.json")]
    else:
        print(f"нет {two / 'ожидания А.json'}: игры two-games не играются (forge_editor --self-test two-games их делает)", flush=True)
    slice_dir = ROOT / "games" / "slice"
    away = Path(tempfile.mkdtemp(prefix="forge_slice_away_")) / "slice"
    shutil.move(str(slice_dir), str(away))
    failed = 0
    try:
        for exe in games:
            for folder, edits in plays:
                user = Path(tempfile.mkdtemp(prefix="forge_author_game_user_"))
                args = [str(exe), "--test", "--scene", "project", "--play", "--data", str(folder / "game"),
                        "--level", str(folder / "game" / "level"), "--user", str(user)]
                if edits:
                    args += ["--edits", str(edits)]
                print("$ " + " ".join(f'"{a}"' if " " in a else a for a in args), flush=True)
                code = subprocess.run(args, cwd=tempfile.gettempdir()).returncode
                print(f"exit {code}", flush=True)
                failed += code != 0
    finally:
        shutil.move(str(away), str(slice_dir))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
