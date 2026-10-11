#!/usr/bin/env python3
"""The core names no module (step 14.3a).

A module is a folder of apps/ with a module/ folder in it (apps/slice, apps/probe): its one public header is
module/<id>_module.h, everything else in it is the module's own. The core is engine/, the launcher apps/game and the
editor apps/editor. This fails when a file of the core

  - includes a header of a module's folder (by its name: "slice_level.h", "probe_module.h", ...), or
  - names a module's namespace in its code ("slice::", "probe::"; comments and strings aside),

except apps/editor/slice_checks.cpp, the editor's self-test's one look into «Старая шахта». The composition root
apps/builtin may include each module's public header only. The build has the same boundary (the modules' folders are no
include folders of the core's targets); this says which file and line crossed it.

    python3 tools/check_core_includes.py [repository]
"""

import re
import sys
from pathlib import Path

CORE = ["engine", "apps/game", "apps/editor"]
ROOT_OF_MODULES = "apps/builtin"
EXCEPTIONS = {"apps/editor/slice_checks.cpp"}
SOURCES = {".h", ".hpp", ".cpp", ".cc", ".inl"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


def code_only(text: str) -> str:
    """The text with comments and string literals blanked out (lines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif text.startswith('R"', i):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            end = ")" + m.group(1) + '"'
            j = text.find(end, i + m.end())
            j = n if j < 0 else j + len(end)
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(" ")
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main() -> int:
    repo = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
    modules = sorted(p.parent for p in (repo / "apps").glob("*/module") if p.is_dir())
    if not modules:
        print("check_core_includes: no module folders in apps/ (apps/*/module)")
        return 1
    ids = [m.name for m in modules]
    own_headers, public_headers = {}, set()
    for m in modules:
        for h in m.rglob("*"):
            if h.suffix in {".h", ".hpp"}:
                if h.parent.name == "module":
                    public_headers.add(h.name)
                else:
                    own_headers[h.name] = m.name
    names = re.compile(r"\b(" + "|".join(map(re.escape, ids)) + r")::")
    problems, files = [], 0

    def scan(path: Path, allowed: set):
        nonlocal files
        rel = path.relative_to(repo).as_posix()
        if rel in EXCEPTIONS:
            return
        files += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        for no, line in enumerate(text.splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            name = m.group(1).split("/")[-1]
            if name in own_headers:
                problems.append(f"{rel}:{no}: includes {m.group(1)}, a header of the module «{own_headers[name]}»")
            elif name in public_headers and name not in allowed:
                problems.append(f"{rel}:{no}: includes {m.group(1)}, a module's header: only {ROOT_OF_MODULES} names modules")
        for no, line in enumerate(code_only(text).splitlines(), 1):
            for m in names.finditer(line):
                if allowed and m.group(0) in {i + "::" for i in ids} and "module_def" in line:
                    continue
                problems.append(f"{rel}:{no}: names the module's namespace {m.group(0)}")

    for part in CORE:
        for path in sorted((repo / part).rglob("*")):
            if path.suffix in SOURCES and path.is_file():
                scan(path, set())
    for path in sorted((repo / ROOT_OF_MODULES).rglob("*")):
        if path.suffix in SOURCES and path.is_file():
            scan(path, public_headers)
    for p in problems:
        print(p)
    print(f"check_core_includes: {files} files of the core and the composition root, modules {', '.join(ids)}: "
          f"{'nothing crosses' if not problems else str(len(problems)) + ' crossings'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
