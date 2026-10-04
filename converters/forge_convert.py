"""Runs one converter for the Forge editor.

    python forge_convert.py job.json

job.json: {"converter": "psd_layers", "input": "...", "output": "...",
           "settings": {...}}

The converter is converters/<converter>.py with a function

    convert(source: Path, out: Path, settings: dict, report) -> list[Path]

that writes its results into the (empty) folder `out` and returns them;
report(progress, message) tells how far it is (progress 0..1). The editor
moves the results next to the source file.

Every line printed is one JSON object: {"progress": 0.5, "message": "..."}
while working, then {"ok": true, "files": [...]} or {"ok": false,
"error": "..."}. Messages are for the author, so they are in Russian.
"""

import importlib
import json
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent


def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) != 2:
        emit({"ok": False, "error": "нужен один аргумент: файл задания"})
        return 2
    job = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    sys.path.insert(0, str(Path(job.get("folder") or HERE)))
    name = job["converter"]
    try:
        module = importlib.import_module(name)
    except ImportError as e:
        missing = getattr(e, "name", None) or str(e)
        emit({"ok": False, "error": f"конвертеру «{name}» не хватает библиотеки Python: {missing}"})
        return 1

    def report(progress, message=""):
        emit({"progress": max(0.0, min(1.0, float(progress))), "message": message})

    out = Path(job["output"])
    out.mkdir(parents=True, exist_ok=True)
    try:
        files = module.convert(Path(job["input"]), out, job.get("settings") or {}, report)
    except ConvertError as e:
        emit({"ok": False, "error": str(e)})
        return 1
    except Exception as e:  # a bug in the converter: say what, keep the details
        emit({"ok": False, "error": f"{type(e).__name__}: {e}", "trace": traceback.format_exc()})
        return 1
    emit({"ok": True, "files": [str(f) for f in files]})
    return 0


class ConvertError(Exception):
    """A problem with the file itself, said in plain words."""


# Converters import it as forge_convert.ConvertError.
sys.modules.setdefault("forge_convert", sys.modules[__name__])

if __name__ == "__main__":
    sys.exit(main())
