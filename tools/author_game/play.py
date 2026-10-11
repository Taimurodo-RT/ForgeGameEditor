"""Plays the author's game «Игра А» that `forge_editor --self-test new-game` made
of the template «Старая шахта», in each game given, with games/slice,
games/platformer and games/modules moved away for the run: the game must take
everything of it from the game's own folder. They come back after it, every
file with the SHA-256 it had (checked), also when a folder does not move or a
game does not start; one that does not come back is named with where it is,
and the others are still moved back.

    python3 tools/author_game/play.py build/dist/OldMine/OldMine build/apps/game/forge_game [build/apps/slice/forge_slice]

Each game runs `--test --scene project --play --data <Игра А>/game --level
<Игра А>/game/level --user <a folder of its own>` and must exit with 0.

Then, when `forge_editor --self-test two-games` has made them (step 14.1b), its
«Игра А» and «Игра Б» the same way, each with `--edits` of what the editor
wrote it must have: A's «Находка» (its picture on the level, the value, the
window, the button's sound), none of it in B. And when `forge_editor
--self-test levels` has made it (step 14.2a), «Игра с уровнями» with no
--level: the game starts its start level, as a player starts it, and must have
the cell the author left there. Then (step 14.2b) it goes through the links
«уходит через» the author made between its levels and is saved; another run,
with the same player's folder, from another working folder and without
--play, takes «Продолжить» and finds the level as the hero left it. And when
`forge_editor --self-test platformer` has made it (step 14.2c), «Платформер А»
with `--edits` of what the author set in the editor: its enemy, coin, spikes,
zone «падает в», flag «доходит до», HUD and windows of the game's end. And when
`forge_editor --self-test platformer-template` has made it (step 14.2d), «Платформер
А» of the catalog's template «Платформер» with no --level and `--edits` of what the
author changed: the hero's picture, the coin's «Очки», the beetle's «Скорость», a
coin and a bridge on «Луг», the exit's link and the win window's title. And when
`forge_editor --self-test modules` has made them (step 14.3a), «Старая игра» (a
game from before step 14.3: no project.forge, game.json without "module") plays as
«Старая шахта», and the saves of two modules meet in one player's folder, each run
another process: the test module's game saves (probe_save), «Старая шахта» refuses
that slot and the slots whose module is none, loads a slot from before step 14.3a
and saves (module_slots), the test module's game refuses those and loads its own
(probe_foreign). This plays a game's own folder with a game program (OldMine of
the template, or the build's: the launcher forge_game, or its old name
forge_slice); it is not an export of the author's game.
"""

import hashlib
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
    plays = [(game, None, True)]
    if (two / "ожидания А.json").is_file() and (two / "ожидания Б.json").is_file():
        plays += [(two / "Мои игры" / "Игра А", two / "ожидания А.json", True), (two / "Мои игры" / "Игра Б", two / "ожидания Б.json", True)]
    else:
        print(f"нет {two / 'ожидания А.json'}: игры two-games не играются (forge_editor --self-test two-games их делает)", flush=True)
    # The game of levels: no --level, its start level.
    levels = Path(tempfile.gettempdir()) / "forge_editor_уровни"
    if (levels / "ожидания старт.json").is_file():
        plays += [(levels / "Мои игры" / "Игра с уровнями", levels / "ожидания старт.json", False)]
    else:
        print(f"нет {levels / 'ожидания старт.json'}: игра с уровнями не играется (forge_editor --self-test levels её делает)", flush=True)
    # The platformer the author made in the editor's tabs.
    platformer = Path(tempfile.gettempdir()) / "forge_editor_платформер"
    if (platformer / "ожидания платформер.json").is_file():
        plays += [(platformer / "Мои игры" / "Платформер А", platformer / "ожидания платформер.json", True)]
    else:
        print(f"нет {platformer / 'ожидания платформер.json'}: платформер автора не играется (forge_editor --self-test platformer его делает)", flush=True)
    # The game of the catalog's «Платформер» the author changed: no --level, its start level.
    template = Path(tempfile.gettempdir()) / "forge_editor_шаблон платформер"
    if (template / "ожидания шаблон.json").is_file():
        plays += [(template / "Мои игры" / "Платформер А", template / "ожидания шаблон.json", False)]
    else:
        print(f"нет {template / 'ожидания шаблон.json'}: шаблон автора не играется (forge_editor --self-test platformer-template его делает)", flush=True)
    going = (levels / "ожидания переход.json").is_file() and (levels / "ожидания продолжить.json").is_file()
    if not going:
        print(f"нет {levels / 'ожидания переход.json'}: переходы между уровнями не играются (forge_editor --self-test levels их делает)", flush=True)
    # The games of the step 14.3a's part modules: a game from before step 14.3 and the test module's.
    modules = Path(tempfile.gettempdir()) / "forge_editor модули" / "Мои игры"
    old, probe = modules / "Старая игра", modules / "Игра пробы"
    two_modules = (old / "game" / "game.json").is_file() and (probe / "game" / "game.json").is_file()
    if two_modules:
        plays += [(old, None, True)]
    else:
        print(f"нет {old}: игры модулей не играются (forge_editor --self-test modules их делает)", flush=True)
    # The sources the games must do without, moved away; their files' SHA-256 to compare when they are back. The
    # folder they wait in is next to them, on the same disk: a move is a rename, nothing is copied half-way.
    sources = [ROOT / "games" / name for name in ("slice", "platformer", "modules")]
    before = {src: digests(src) for src in sources}
    aside = Path(tempfile.mkdtemp(prefix="forge_sources_away_", dir=ROOT))
    moved = []  # (source, where it waits): only those moved, so only those are moved back
    failed = 0

    def run(args, cwd) -> int:
        print("$ " + " ".join(f'"{a}"' if " " in a else a for a in args), flush=True)
        code = subprocess.run(args, cwd=cwd).returncode
        print(f"exit {code}", flush=True)
        return code

    try:
        for src in sources:
            shutil.move(str(src), str(aside / src.name))
            moved.append((src, aside / src.name))
        for exe in games:
            for folder, edits, level in plays:
                user = Path(tempfile.mkdtemp(prefix="forge_author_game_user_"))
                args = [str(exe), "--test", "--scene", "project", "--play", "--data", str(folder / "game")]
                if level:
                    args += ["--level", str(folder / "game" / "level")]
                args += ["--user", str(user)]
                if edits:
                    args += ["--edits", str(edits)]
                failed += run(args, tempfile.gettempdir()) != 0
            if going:
                # Through the links, saved; then «Продолжить» by another run, from another working folder.
                user = Path(tempfile.mkdtemp(prefix="forge_author_game_user_"))
                data = str(levels / "Мои игры" / "Игра с уровнями" / "game")
                failed += run([str(exe), "--test", "--scene", "project", "--play", "--data", data, "--user", str(user),
                               "--edits", str(levels / "ожидания переход.json")], tempfile.gettempdir()) != 0
                elsewhere = Path(tempfile.mkdtemp(prefix="forge_author_game_cwd_"))
                failed += run([str(exe), "--test", "--scene", "project", "--data", data, "--user", str(user),
                               "--edits", str(levels / "ожидания продолжить.json")], str(elsewhere)) != 0
            if two_modules:
                # Two modules' saves in one player's folder, each run another process, each from another working folder.
                user = Path(tempfile.mkdtemp(prefix="forge_author_game_user_"))
                for scene, data in (("probe_save", probe), ("module_slots", old), ("probe_foreign", probe)):
                    elsewhere = Path(tempfile.mkdtemp(prefix="forge_author_game_cwd_"))
                    failed += run([str(exe), "--test", "--scene", scene, "--data", str(data / "game"), "--user", str(user)], str(elsewhere)) != 0
    except Exception as e:  # a folder that does not move, a game that does not start: the folders come back all the same
        print(f"прогон прерван: {type(e).__name__}: {e}", flush=True)
        failed += 1
    finally:
        failed += restore(moved)
    for src in sources:
        after = digests(src)
        same = after == before[src]
        print(f"{src.relative_to(ROOT).as_posix()}: {len(after)} файлов {'на месте с теми же SHA-256' if same else 'НЕ на месте или НЕ такие же'}", flush=True)
        failed += not same
    try:
        aside.rmdir()  # empty when everything came back
    except OSError:
        print(f"папка {aside} не пуста: в ней то, что не вернулось", flush=True)
    return 1 if failed else 0


def restore(moved) -> int:
    """Moves each folder back to where it was; one that does not come back does not stop the others. The number that did not."""
    lost = 0
    for src, waits in reversed(moved):
        try:
            shutil.move(str(waits), str(src))
        except Exception as e:
            print(f"НЕ ВОЗВРАЩЕНА {src}: она в {waits} ({type(e).__name__}: {e})", flush=True)
            lost += 1
    return lost


def digests(folder: Path) -> dict:
    """SHA-256 of every file of the folder, by its path in it."""
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.rglob("*")) if p.is_file()}


if __name__ == "__main__":
    sys.exit(main())
