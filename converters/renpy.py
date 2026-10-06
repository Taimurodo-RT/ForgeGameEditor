"""Ren'Py → разговоры Forge.

Reads a Ren'Py game (every .rpy next to the chosen file and below it) and
writes one conversation per script file (dialogues/*.json, see
engine/game/include/forge/game/dialogue.h):

- characters (define e = Character("Эйлин", who_color=...)) become speakers;
- say statements become lines; attributes (y 1u "...") become the pose;
- menus become the hero's answers, if/elif/else and while become branches;
- labels become scenes; jump and call go to them, also into other files
  ("ch1:ch1_main"); return comes back from a call;
- $ lines and python blocks become actions when they are plain variables
  (x = 1, x += 1, persistent.seen = True); anything else is kept word for
  word as renpy("...") so nothing is lost and the editor can show it;
- scene, show, hide, with, play, stop, pause... are kept as the line's
  staging ("stage"), for the visual novel module.

Two more files sit next to the talks (their names have a dot, so no talk
can take them):
- characters.cast.json, the cast: every Character(...) with its name,
  colour, picture tag and poses (image monika 1a = ...), its other settings
  as written (what_prefix, ...), how many lines it says and in which talks;
- novel.settings.json, the game's settings, read and kept for the visual
  novel module to take on: the system files (options.rpy, gui.rpy,
  screens.rpy) are not talks, their define config./gui./build. values, the
  screen size, music names (define audio.t1 = "bgm/1.ogg"), screens,
  transforms and pictures are listed there.

What could not be carried over is listed in «Импорт Ren'Py.txt».
"""

from __future__ import annotations

import ast
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

try:
    from forge_convert import ConvertError
except ImportError:  # run on its own (tests)
    class ConvertError(Exception):
        pass


PALETTE = ["#e8b04a", "#7ec8a0", "#e07a6a", "#8fb3e8", "#c79be0", "#e6c86e", "#6fc3c9", "#e89bbb"]

STAGING = {"scene", "show", "hide", "with", "play", "stop", "queue", "voice", "pause", "window", "nvl", "camera", "sound"}
# Settings and screens, not story: never talks (their values go to the settings).
SYSTEM_FILES = {"options.rpy", "gui.rpy", "screens.rpy"}
SETTING_GROUPS = [("config.", "Игра"), ("build.", "Сборка"), ("gui.", "Внешний вид"), ("audio.", "Музыка и звуки"),
                  ("style.", "Стили"), ("", "Другое")]
SKIPPED_IN_LABELS = {"image", "define", "default", "transform", "style", "screen", "init", "translate", "layeredimage"}


# --- reading ------------------------------------------------------------------

@dataclass
class Line:
    text: str
    indent: int
    file: str
    number: int
    block: list["Line"] = field(default_factory=list)


def logical_lines(source: str, file: str):
    """Physical lines joined while a bracket or a string is open; comments dropped."""
    out = []
    buf, start, indent = "", 0, 0
    depth, quote = 0, ""
    for number, raw in enumerate(source.splitlines(), 1):
        if not buf:
            stripped = raw.lstrip(" \t")
            if not stripped or stripped.startswith("#"):
                continue
            indent = len(raw.expandtabs(4)) - len(stripped.expandtabs(4))
            start = number
            raw = stripped
        else:
            buf += "\n"
        i = 0
        while i < len(raw):
            c = raw[i]
            if quote:
                if c == "\\":
                    i += 2
                    continue
                if raw.startswith(quote, i):
                    i += len(quote)
                    quote = ""
                    continue
            elif c == "#":
                raw = raw[:i].rstrip()
                break
            elif c in "\"'`":
                quote = raw[i:i + 3] if raw[i:i + 3] in ('"""', "'''") else c
                i += len(quote)
                continue
            elif c in "([{":
                depth += 1
            elif c in ")]}":
                depth = max(0, depth - 1)
            i += 1
        buf += raw
        if depth == 0 and not quote:
            if buf.strip():
                out.append(Line(buf.strip(), indent, file, start))
            buf = ""
    if buf.strip():
        out.append(Line(buf.strip(), indent, file, start))
    return out


def tree(lines):
    """Lines into blocks by indentation."""
    root = Line("", -1, "", 0)
    stack = [root]
    for line in lines:
        while stack[-1].indent >= line.indent and len(stack) > 1:
            stack.pop()
        stack[-1].block.append(line)
        stack.append(line)
    return root.block


STRING = r'(?:"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|`(?:[^`\\]|\\.)*`)'
TRIPLE = r'(?:"""[\s\S]*?"""|\'\'\'[\s\S]*?\'\'\')'


def tokens(text: str):
    """Words and strings of a statement (strings keep their quotes)."""
    return re.findall(TRIPLE + "|" + STRING + r"|[\w.]+|\S", text)


def unquote(s: str) -> str:
    if s[:3] in ('"""', "'''"):
        body = s[3:-3]
        body = re.sub(r"\s*\n\s*", " ", body.strip())
    else:
        body = s[1:-1]
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            out.append({"n": "\n", "t": "\t"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def bare(text: str) -> str:
    """A statement without its init priority: "init -501 screen say" → "screen say",
    "define -2 gui.x = 1" → "define gui.x = 1" (decompiled games write them)."""
    text = re.sub(r"^init\s+-?\d+\s+(?=[a-z])", "", text)
    return re.sub(r"^(define|default)\s+-?\d+\s+", r"\1 ", text)


def is_string(tok: str) -> bool:
    return tok[:1] in "\"'`"


# --- text ---------------------------------------------------------------------

TAG = re.compile(r"\{(/?)([a-z_]+)(=[^}]*)?\}")


def convert_text(text: str, notes) -> str:
    """Ren'Py text into Forge text: [var] → {var}, text tags dropped."""
    text = text.replace("{{", "\x01").replace("[[", "\x02").replace("%%", "%")

    def tag(m):
        notes.tags += 1
        return ""

    text = TAG.sub(tag, text)

    def var(m):
        name = m.group(1).split("!")[0].split(":")[0].strip()
        name = re.sub(r"\[(\w+)\]", r".\1", name)
        return "{" + name + "}"

    text = re.sub(r"\[([^\[\]]+)\]", var, text)
    return text.replace("\x01", "(").replace("\x02", "[")


# --- python -------------------------------------------------------------------

class Untranslatable(Exception):
    pass


OPS = {ast.Eq: "==", ast.NotEq: "!=", ast.Lt: "<", ast.LtE: "<=", ast.Gt: ">", ast.GtE: ">="}
BIN = {ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/"}


def py_name(node) -> str:
    if isinstance(node, ast.Name):
        if node.id in ("True", "False", "None"):
            raise Untranslatable
        return node.id
    if isinstance(node, ast.Attribute):
        return py_name(node.value) + "." + node.attr
    if isinstance(node, ast.Subscript):
        index = node.slice
        if isinstance(index, ast.Index):  # Python 3.8
            index = index.value
        if isinstance(index, ast.Constant) and isinstance(index.value, (int, str)) and not isinstance(index.value, bool):
            return py_name(node.value) + "." + str(index.value)
    raise Untranslatable


def py_expr(node) -> str:
    if isinstance(node, ast.Constant):
        v = node.value
        if v is True:
            return "true"
        if v is False or v is None:
            return "false"
        if isinstance(v, (int, float)):
            return repr(v)
        if isinstance(v, str):
            return json.dumps(v, ensure_ascii=False)
        raise Untranslatable
    if isinstance(node, ast.Name) and node.id in ("True", "False", "None"):
        return {"True": "true", "False": "false", "None": "false"}[node.id]
    if isinstance(node, (ast.Name, ast.Attribute, ast.Subscript)):
        return py_name(node)
    if isinstance(node, ast.BoolOp):
        op = " and " if isinstance(node.op, ast.And) else " or "
        return "(" + op.join(py_expr(v) for v in node.values) + ")"
    if isinstance(node, ast.UnaryOp):
        if isinstance(node.op, ast.Not):
            return "not " + py_expr(node.operand)
        if isinstance(node.op, ast.USub):
            return "(0 - " + py_expr(node.operand) + ")"
        raise Untranslatable
    if isinstance(node, ast.BinOp) and type(node.op) in BIN:
        return "(" + py_expr(node.left) + " " + BIN[type(node.op)] + " " + py_expr(node.right) + ")"
    if isinstance(node, ast.Compare):
        parts, left = [], node.left
        for op, right in zip(node.ops, node.comparators):
            if isinstance(op, (ast.Is, ast.IsNot)) and isinstance(right, ast.Constant) and right.value is None:
                parts.append(py_expr(left) + (" == 0" if isinstance(op, ast.Is) else " != 0"))
            elif type(op) in OPS:
                parts.append(py_expr(left) + " " + OPS[type(op)] + " " + py_expr(right))
            else:
                raise Untranslatable
            left = right
        return parts[0] if len(parts) == 1 else "(" + " and ".join(parts) + ")"
    raise Untranslatable


def strip_parens(s: str) -> str:
    while s.startswith("(") and s.endswith(")"):
        depth = 0
        for i, c in enumerate(s):
            depth += c == "("
            depth -= c == ")"
            if depth == 0 and i < len(s) - 1:
                return s
        s = s[1:-1]
    return s


def condition(src: str) -> str | None:
    try:
        return strip_parens(py_expr(ast.parse(src.strip(), mode="eval").body))
    except (Untranslatable, SyntaxError, RecursionError):
        return None


def statements(src: str) -> list[str] | None:
    try:
        module = ast.parse(src.strip())
    except SyntaxError:
        return None
    out = []
    try:
        for st in module.body:
            if isinstance(st, ast.Assign) and len(st.targets) == 1:
                out.append(py_name(st.targets[0]) + " = " + strip_parens(py_expr(st.value)))
            elif isinstance(st, ast.AnnAssign) and st.value is not None:
                out.append(py_name(st.target) + " = " + strip_parens(py_expr(st.value)))
            elif isinstance(st, ast.AugAssign) and isinstance(st.op, (ast.Add, ast.Sub)):
                out.append(py_name(st.target) + (" += " if isinstance(st.op, ast.Add) else " -= ") + strip_parens(py_expr(st.value)))
            elif isinstance(st, ast.AugAssign) and type(st.op) in BIN:
                t = py_name(st.target)
                out.append(t + " = " + t + " " + BIN[type(st.op)] + " " + py_expr(st.value))
            elif isinstance(st, ast.Pass):
                continue
            else:
                raise Untranslatable
    except (Untranslatable, RecursionError):
        return None
    return out


def raw_call(src: str) -> str:
    return "renpy(" + json.dumps(src.strip(), ensure_ascii=False) + ")"


def presentation(src: str) -> bool:
    """Python that only changes what is on screen or heard."""
    s = src.strip()
    return bool(re.match(r"(pause\(|renpy\.(pause|show|hide|music|sound|scene|transition|with_statement|play|stop|window|"
                         r"block_rollback|fix_rollback|checkpoint|hide_screen|show_screen|notify)|"
                         r"config\.|_window|quick_menu|allow_skipping|style\.|gui\.)", s))


# --- the project --------------------------------------------------------------

@dataclass
class Notes:
    lines: int = 0
    tags: int = 0
    raw_actions: list = field(default_factory=list)
    raw_conditions: list = field(default_factory=list)
    skipped: list = field(default_factory=list)
    unknown_labels: list = field(default_factory=list)

    def where(self, line: Line) -> str:
        return f"{line.file}:{line.number}"


@dataclass
class Character:
    key: str
    name: str
    color: str
    image: str
    name_var: str = ""  # DynamicCharacter: the variable holding the name
    options: dict = field(default_factory=dict)  # its other settings, as written
    lines: int = 0
    talks: set = field(default_factory=set)


def call_args(src: str):
    """Arguments of Character(...) / DynamicCharacter(...)."""
    try:
        node = ast.parse(src.strip(), mode="eval").body
    except SyntaxError:
        return None
    if not isinstance(node, ast.Call):
        return None
    fn = node.func.id if isinstance(node.func, ast.Name) else node.func.attr if isinstance(node.func, ast.Attribute) else ""
    pos = [a.value if isinstance(a, ast.Constant) else None for a in node.args]
    kw = {k.arg: k.value.value for k in node.keywords if k.arg and isinstance(k.value, ast.Constant)}
    # Settings that are not plain values (what_prefix=..., ctc=anim) as written.
    for k in node.keywords:
        if k.arg and k.arg not in kw:
            kw[k.arg] = "= " + ast.unparse(k.value)
    return fn, pos, kw


class Project:
    def __init__(self, files: dict[str, str]):
        self.notes = Notes()
        self.trees = {name: tree(logical_lines(text, name)) for name, text in files.items()}
        self.characters: dict[str, Character] = {}
        self.text_values: dict[str, str] = {}
        self.defaults: list[str] = []
        self.label_file: dict[str, str] = {}
        self.images: dict[str, list[str]] = {}  # picture tag → its attributes ("1a", "happy")
        self.settings: list[tuple[str, str, str]] = []  # (name, value as written, file)
        self.screens: list[str] = []
        self.transforms: list[str] = []
        self.styles = 0
        self.size: list[int] = []
        self._collect()

    @staticmethod
    def talk_id(file: str) -> str:
        stem = Path(file).with_suffix("").as_posix().replace("/", "_")
        return re.sub(r"[^\w\-]", "_", stem)

    # Pass 1: characters, names, labels.
    def _collect(self):
        dynamic = {}
        for file, lines in self.trees.items():
            for line in self._walk(lines):
                t = bare(line.text)
                m = re.match(r"(?:(?:define|default|\$)\s+)?([\w.]+)\s*=\s*(.+)$", t, re.S)
                if m:
                    var, value = m.group(1), m.group(2)
                    args = call_args(value)
                    if args and args[0] in ("Character", "DynamicCharacter", "ADVCharacter", "NVLCharacter"):
                        fn, pos, kw = args
                        name = pos[0] if pos and isinstance(pos[0], str) else kw.get("name") or ""
                        if fn == "DynamicCharacter" or kw.get("dynamic"):
                            dynamic[var] = name
                            name = ""
                        color = kw.get("who_color") or kw.get("color") or ""
                        image = kw.get("image") or ""
                        options = {k: v for k, v in kw.items() if k not in ("name", "who_color", "color", "image", "dynamic")}
                        self.characters[var] = Character(var, name, color if isinstance(color, str) and not color.startswith("= ") else "",
                                                         image if isinstance(image, str) and not image.startswith("= ") else "",
                                                         dynamic.get(var, ""), options)
                        continue
                    try:
                        v = ast.literal_eval(value.strip())
                    except Exception:
                        v = None
                    if isinstance(v, str) and v:
                        self.text_values.setdefault(var, v)
                    # A missing variable reads as 0, and persistent data is kept between
                    # runs, so only other non-zero defaults need setting at the start.
                    if t.startswith("default") and not var.startswith("persistent.") and v not in (0, False, None, "", [], {}):
                        st = statements(f"{var} = {value}")
                        if st:
                            self.defaults += st
            self._collect_settings(file, lines)
            parent = ""
            for line in self._story(lines):
                lm = re.match(r"label\s+([\w.]+)", line.text)
                if not lm:
                    continue
                name = lm.group(1)
                if name.startswith("."):  # a local label: parent.name
                    name = parent + name
                else:
                    parent = name.split(".")[0]
                self.label_file.setdefault(name, file)
        for var, name_var in dynamic.items():
            c = self.characters[var]
            c.name_var = name_var
            c.name = self.text_values.get(name_var) or ("{" + name_var + "}" if name_var else var)
        for i, c in enumerate(sorted(self.characters.values(), key=lambda c: c.key)):
            if not c.color or not re.match(r"#[0-9a-fA-F]{3,8}$", c.color):
                c.color = PALETTE[i % len(PALETTE)]
            if len(c.color) == 4:
                c.color = "#" + "".join(ch * 2 for ch in c.color[1:])
            c.color = c.color[:7]

    def _collect_settings(self, file: str, lines):
        """Pictures, screens, transforms, styles and define config./gui./... values."""
        for line in self._walk(lines):
            t = bare(line.text)
            m = re.match(r"image\s+([\w ]+?)\s*(?:=|:$)", t)
            if m:
                words = m.group(1).split()
                self.images.setdefault(words[0], []).append(" ".join(words[1:]))
                continue
            m = re.match(r"screen\s+(\w+)", t)
            if m:
                self.screens.append(m.group(1))
                continue
            m = re.match(r"transform\s+(\w+)", t)
            if m:
                self.transforms.append(m.group(1))
                continue
            if re.match(r"style\s+\w+", t):
                self.styles += 1
                continue
            m = re.search(r"gui\.init\(\s*(\d+)\s*,\s*(\d+)\s*\)", t)
            if m and not self.size:
                self.size = [int(m.group(1)), int(m.group(2))]
            m = re.match(r"define\s+((?:config|gui|build|audio|style)\.[\w.]+)\s*=\s*(.+)$", t, re.S)
            if m:
                self.settings.append((m.group(1), " ".join(m.group(2).split()), file))

    def setting(self, name: str):
        """A setting's plain value (a string, a number, True/False), else None."""
        for key, value, _ in self.settings:
            if key == name:
                try:
                    v = ast.literal_eval(value)
                except Exception:
                    m = re.fullmatch(r"_\((.*)\)", value)  # _("text"): translatable
                    try:
                        v = ast.literal_eval(m.group(1)) if m else None
                    except Exception:
                        v = None
                return v
        return None

    def cast_json(self) -> dict:
        chars = []
        for c in sorted(self.characters.values(), key=lambda c: (-c.lines, c.key)):
            poses = self.images.get(c.image, []) if c.image else []
            chars.append({"key": c.key, "name": c.name, "color": c.color, "image": c.image, "name_var": c.name_var,
                          "poses": [p for p in poses if p], "lines": c.lines, "talks": sorted(c.talks), "options": c.options})
        return {"characters": chars}

    def settings_json(self, files: list[str]) -> dict:
        groups = {title: [] for _, title in SETTING_GROUPS}
        for key, value, file in self.settings:
            title = next(t for prefix, t in SETTING_GROUPS if key.startswith(prefix))
            groups[title].append({"name": key, "value": value, "file": file})
        out = {"title": self.setting("config.name") or "", "short": self.setting("build.name") or "",
               "version": str(self.setting("config.version") or ""), "size": self.size,
               "groups": [{"name": t, "items": items} for t, items in groups.items() if items],
               "screens": sorted(set(self.screens)), "transforms": sorted(set(self.transforms)), "styles": self.styles,
               "images": sum(len(v) for v in self.images.values()), "files": sorted(files)}
        return out

    def _walk(self, lines):
        for line in lines:
            yield line
            yield from self._walk(line.block)

    def _story(self, lines):
        """Lines of the story itself: not inside screens, styles, init blocks."""
        for line in lines:
            head = (tokens(line.text) or [""])[0]
            if head in SKIPPED_IN_LABELS or head == "python":
                continue
            yield line
            yield from self._story(line.block)

    def talks(self):
        out = {}
        for file, lines in self.trees.items():
            if Path(file).name in SYSTEM_FILES or not any(re.match(r"label\s", l.text) for l in lines):
                continue
            talk = Talk(self, file)
            talk.compile(lines)
            if not talk.nodes:
                continue
            out[self.talk_id(file)] = talk.json()
        return out

    def ref(self, label: str, file: str, line: Line) -> str:
        where = self.label_file.get(label)
        if where is None:
            self.notes.unknown_labels.append(f"{self.notes.where(line)}: метка «{label}»")
            return "end"
        return label if where == file else self.talk_id(where) + ":" + label


# --- compiling one file -------------------------------------------------------

class Talk:
    def __init__(self, project: Project, file: str):
        self.p = project
        self.file = file
        self.nodes: list[dict] = []
        self.ids: set[str] = set()
        self.pend: list = []  # (dict, key) whose target is the next node made
        self.stage: list[str] = []
        self.actions: list[str] = []
        self.label = Path(file).stem
        self.counter = 0
        self.last_say = None
        self.speakers_used: set[str] = set()

    # Nodes.
    def fresh(self, stem: str) -> str:
        while True:
            self.counter += 1
            nid = f"{stem}_{self.counter}"
            if nid not in self.ids and nid not in self.p.label_file:
                return nid

    def link(self, target: str):
        for obj, key in self.pend:
            obj[key] = target
        self.pend = []

    def node(self, nid: str | None = None, **fields) -> dict:
        n = {"id": nid or self.fresh(self.label)}
        self.ids.add(n["id"])
        if self.stage:
            n["stage"] = self.stage
            self.stage = []
        if self.actions:
            n["do"] = self.actions
            self.actions = []
        n.update(fields)
        self.link(n["id"])
        self.nodes.append(n)
        self.last_say = None
        return n

    def flush(self):
        """Staging and actions waiting for a line, put on a node of their own."""
        if self.stage or self.actions:
            n = self.node()
            self.pend = [(n, "next")]

    # Statements.
    def compile(self, lines):
        for line in lines:
            if re.match(r"label\s", line.text):
                self.statement(line)
            # Anything else at the top of a file (definitions) is not part of a talk.
        self.flush()
        self.link("return")

    def block(self, lines):
        for line in lines:
            self.statement(line)

    def statement(self, line: Line):
        t = line.text
        toks = tokens(t)
        if not toks:
            return
        head = toks[0]
        notes = self.p.notes

        if head == "label":
            m = re.match(r"label\s+([\w.]+)", t)
            name = m.group(1)
            if name.startswith("."):
                name = self.label.split(".")[0] + name
            self.flush()
            self.label = name
            self.counter = 0
            n = self.node(name, scene=name)
            self.pend = [(n, "next")]
            self.block(line.block)
            return
        if head == "menu":
            self.menu(line)
            return
        if head == "if":
            self.branches(line)
            return
        if head in ("elif", "else"):
            return  # handled with their if
        if head == "while":
            self.loop(line)
            return
        if head == "jump":
            target = self.target(toks[1:], line, "jump")
            if target is None:
                return
            self.flush()
            self.link(target)
            return
        if head == "call" and len(toks) > 1 and toks[1] != "screen":
            target = self.target(toks[1:], line, "call")
            if target is None:
                return
            n = self.node(call=target)
            self.pend = [(n, "next")]
            return
        if head == "return":
            self.flush()
            self.link("return")
            return
        if head == "pass":
            return
        if head == "$":
            self.python(t[1:].strip(), line)
            return
        if head in ("python", "init") and t.endswith(":"):
            if head == "init":
                notes.skipped.append(f"{notes.where(line)}: init внутри метки")
                return
            src = "\n".join(self.source_of(line.block))
            self.python(src, line)
            return
        if head in STAGING or (head == "call" and len(toks) > 1 and toks[1] == "screen"):
            if head == "call":  # an interactive screen (a mini-game): kept as Ren'Py
                self.actions.append(raw_call(t))
                notes.raw_actions.append(f"{notes.where(line)}: {t}")
                return
            text = t.rstrip(":")
            if line.block:
                text += " { " + "; ".join(self.source_of(line.block)) + " }"
            self.stage.append(text)
            return
        if head in SKIPPED_IN_LABELS:
            notes.skipped.append(f"{notes.where(line)}: {t.splitlines()[0][:60]}")
            return
        if head == "extend":
            say = self.say_parts(toks[1:])
            if say:
                speaker = self.last_speaker if hasattr(self, "last_speaker") else ""
                self.say(speaker, "", say[1], line)
            return
        say = self.say_parts(toks)
        if say:
            who_and_pose, text = say
            speaker, pose = "", ""
            if who_and_pose:
                first = who_and_pose[0]
                if is_string(first):
                    speaker = self.literal_speaker(unquote(first))
                    pose = " ".join(who_and_pose[1:])
                elif first in self.p.characters:
                    speaker = first
                    pose = " ".join(w for w in who_and_pose[1:] if w not in ("@",))
                else:
                    speaker = self.literal_speaker(first)
                    pose = " ".join(who_and_pose[1:])
            self.say(speaker, pose, text, line)
            return
        # Something this importer does not know: kept word for word.
        self.actions.append(raw_call(t))
        notes.raw_actions.append(f"{notes.where(line)}: {t[:80]}")

    def source_of(self, lines, depth=0):
        out = []
        for l in lines:
            out.append("    " * depth + l.text)
            out += self.source_of(l.block, depth + 1)
        return out

    def target(self, toks, line, what):
        if not toks:
            return None
        if toks[0] == "expression":
            self.actions.append(raw_call(line.text))
            self.p.notes.raw_actions.append(f"{self.p.notes.where(line)}: {line.text}")
            n = self.node()
            if what == "jump":
                self.link("end")  # the next node is not reached from here
                n["next"] = "end"
                self.pend = []
            else:
                self.pend = [(n, "next")]
            return None
        name = toks[0]
        if name.startswith("."):
            name = self.label.split(".")[0] + name
        return self.p.ref(name, self.file, line)

    def say_parts(self, toks):
        """(who and attributes, text) of a say statement, or None."""
        strings = [i for i, tok in enumerate(toks) if is_string(tok)]
        if not strings:
            return None
        # The text is the last string before any clause (with, id, nointeract, ()).
        idx = None
        for i, tok in enumerate(toks):
            if is_string(tok):
                idx = i
            elif tok in ("with", "id", "nointeract", "(") and idx is not None:
                break
        before = toks[:idx]
        if any(not (re.match(r"[\w.@\-]+$", w) or is_string(w)) for w in before):
            return None
        if len(before) > 6:
            return None
        rest = toks[idx + 1:]
        if "with" in rest:
            w = rest.index("with")
            if w + 1 < len(rest):
                self.stage.append("with " + rest[w + 1])
        return before, unquote(toks[idx])

    def literal_speaker(self, name: str) -> str:
        key = "who_" + re.sub(r"\W+", "_", name.lower()).strip("_")
        if key not in self.p.characters:
            self.p.characters[key] = Character(key, name, PALETTE[len(self.p.characters) % len(PALETTE)], "")
        return key

    def say(self, speaker: str, pose: str, text: str, line: Line):
        self.p.notes.lines += 1
        fields = {}
        if speaker:
            fields["speaker"] = speaker
            self.speakers_used.add(speaker)
            c = self.p.characters[speaker]
            c.lines += 1
            c.talks.add(Project.talk_id(self.file))
        if pose:
            fields["pose"] = pose
        fields["text"] = convert_text(text, self.p.notes) or "…"
        n = self.node(**fields)
        self.pend = [(n, "next")]
        self.last_say = n
        self.last_speaker = speaker

    def python(self, src: str, line: Line):
        if not src.strip():
            return
        st = statements(src)
        if st is not None:
            self.actions += st
            return
        if presentation(src) and "\n" not in src.strip():
            self.stage.append("$ " + src.strip())
            return
        self.actions.append(raw_call(src))
        self.p.notes.raw_actions.append(f"{self.p.notes.where(line)}: {src.strip().splitlines()[0][:80]}")

    def cond(self, src: str, line: Line) -> str:
        c = condition(src)
        if c is None:
            self.p.notes.raw_conditions.append(f"{self.p.notes.where(line)}: {src.strip()[:80]}")
            return raw_call(src)
        return c

    def menu(self, line: Line):
        host = None
        choices = []
        for child in line.block:
            ct = child.text
            ctoks0 = tokens(ct)
            if ct.endswith(":") and ctoks0 and is_string(ctoks0[0]):
                choices.append(child)
            elif ct.startswith("set "):
                continue
            elif host is None and not ct.endswith(":"):
                say = self.say_parts(tokens(ct))
                if say:
                    who, text = say
                    speaker = ""
                    if who:
                        speaker = who[0] if who[0] in self.p.characters else self.literal_speaker(unquote(who[0]) if is_string(who[0]) else who[0])
                    self.say(speaker, " ".join(who[1:]) if who else "", text, child)
                    host = self.last_say
        if host is None:
            last = self.last_say
            if last is not None and self.pend == [(last, "next")] and "choices" not in last and not self.stage and not self.actions:
                host = last
                self.pend = []
            else:
                host = self.node(text="…")
        else:
            self.pend = []
        host["choices"] = []
        exits = []
        for child in choices:
            ctoks = tokens(child.text.rstrip(":"))
            text = unquote(ctoks[0])
            entry = {"text": convert_text(text, self.p.notes)}
            m = re.search(r"\bif\b(.+)$", child.text.rstrip(":").strip()[len(ctoks[0]):], re.S)
            if m:
                entry["if"] = self.cond(m.group(1), child)
            host["choices"].append(entry)
            self.pend = [(entry, "goto")]
            self.last_say = None
            self.block(child.block)
            self.flush_if_waiting()
            exits += self.pend
        self.pend = exits
        self.last_say = None

    def flush_if_waiting(self):
        if self.stage or self.actions:
            self.flush()

    def branches(self, line: Line):
        # The if, then its elif / else siblings that follow it.
        parent_block = self.siblings_of(line)
        chain = [line]
        if parent_block is not None:
            i = parent_block.index(line) + 1
            while i < len(parent_block) and tokens(parent_block[i].text)[:1] and tokens(parent_block[i].text)[0] in ("elif", "else") \
                    and parent_block[i].indent == line.indent:
                chain.append(parent_block[i])
                if tokens(parent_block[i].text)[0] == "else":
                    break
                i += 1
        j = self.node(branches=[])
        exits = []
        has_else = False
        for part in chain:
            head = tokens(part.text)[0]
            if head == "else":
                has_else = True
                self.pend = [(j, "next")]
            else:
                src = part.text[len(head):].rstrip()
                src = src[:-1] if src.endswith(":") else src
                entry = {"if": self.cond(src, part)}
                j["branches"].append(entry)
                self.pend = [(entry, "goto")]
            self.last_say = None
            self.block(part.block)
            self.flush_if_waiting()
            exits += self.pend
        if not has_else:
            exits.append((j, "next"))
        self.pend = exits

    def loop(self, line: Line):
        self.flush()
        src = line.text[len("while"):].rstrip()
        src = src[:-1] if src.endswith(":") else src
        entry = {"if": self.cond(src, line)}
        j = self.node(branches=[entry])
        self.pend = [(entry, "goto")]
        self.block(line.block)
        self.flush_if_waiting()
        self.link(j["id"])
        self.pend = [(j, "next")]

    # The block a line sits in, to find its elif / else.
    def siblings_of(self, line: Line):
        def find(lines):
            if line in lines:
                return lines
            for l in lines:
                r = find(l.block)
                if r is not None:
                    return r
            return None
        return find(self.p.trees[self.file])

    def json(self) -> dict:
        speakers = {}
        for key in sorted(self.speakers_used):
            c = self.p.characters[key]
            speakers[key] = {"name": c.name, "color": c.color}
        nodes = self.nodes
        # "end" and "return" are written as such; dangling exits end the talk.
        for n in nodes:
            for key in ("next",):
                if n.get(key) == "end":
                    del n[key]
        start = [{"goto": nodes[0]["id"]}] if nodes else []
        if nodes and "start" in self.ids:
            start = [{"goto": "start"}]
            first = next(n for n in nodes if n["id"] == "start")
            if self.p.defaults:
                first["do"] = self.p.defaults + first.get("do", [])
        return {"id": Project.talk_id(self.file), "speakers": speakers, "start": start, "nodes": nodes}


# --- writing ------------------------------------------------------------------

def read_project(folder: Path) -> dict[str, str]:
    files = {}
    for path in sorted(folder.rglob("*.rpy")):
        if any(part.startswith(".") or part in ("tl", "cache") for part in path.relative_to(folder).parts[:-1]):
            continue
        rel = path.relative_to(folder).as_posix()
        files[rel] = path.read_text(encoding="utf-8-sig", errors="replace")
    return files


def report_text(p: Project, talks: dict) -> str:
    n = p.notes
    lines = [
        "Импорт Ren'Py",
        "",
        f"Разговоров: {len(talks)}, реплик: {n.lines}, говорящих: {len(p.characters)}.",
        f"Текстовых тегов ({{i}}, {{w}} и других) убрано: {n.tags}.",
        "",
    ]

    def part(title, items):
        if not items:
            return
        lines.append(f"{title} ({len(items)}):")
        lines.extend("  " + i for i in items[:400])
        if len(items) > 400:
            lines.append(f"  … и ещё {len(items) - 400}")
        lines.append("")

    part("Код Python, оставленный как есть (renpy(...))", n.raw_actions)
    part("Условия Python, оставленные как есть (renpy(...), в проверке читаются как «нет»)", n.raw_conditions)
    part("Переходы на метки, которых нет в проекте", n.unknown_labels)
    part("Пропущено (описания внутри меток)", n.skipped)
    return "\n".join(lines)


def q(s: str) -> str:
    return json.dumps(s, ensure_ascii=False)


def talk_json(talk: dict) -> str:
    """Laid out the way the editor writes talks (dialogue_source.cpp), so the
    first change in the editor does not rewrite the whole file."""
    def entry(e):
        return "{" + (f'"if": {q(e["if"])}, ' if e.get("if") else "") + f'"goto": {q(e.get("goto") or "end")}' + "}"

    def field(name, value):
        return f", {q(name)}: {q(value)}" if value else ""

    out = "{\n  \"id\": " + q(talk["id"]) + ",\n  \"speakers\": {"
    for i, (key, sp) in enumerate(talk["speakers"].items()):
        out += ",\n               " if i else ""
        out += q(key) + ": {\"name\": " + q(sp["name"]) + field("color", sp.get("color", "")) + "}"
    out += "},\n  \"start\": ["
    out += "".join((",\n    " if i else "\n    ") + entry(e) for i, e in enumerate(talk["start"]))
    out += "\n  ],\n" if talk["start"] else "],\n"
    out += "  \"nodes\": ["
    for i, n in enumerate(talk["nodes"]):
        out += ",\n" if i else "\n"
        out += "    {\"id\": " + q(n["id"])
        out += field("scene", n.get("scene", "")) + field("speaker", n.get("speaker", "")) + field("pose", n.get("pose", ""))
        if n.get("stage"):
            out += ",\n     \"stage\": [" + ", ".join(q(x) for x in n["stage"]) + "]"
        if n.get("text"):
            out += ",\n     \"text\": " + q(n["text"])
        if n.get("do"):
            out += ",\n     \"do\": " + q("; ".join(n["do"]))
        if n.get("branches"):
            out += ",\n     \"branches\": [" + ", ".join(entry(b) for b in n["branches"]) + "]"
        if n.get("choices"):
            out += ",\n     \"choices\": ["
            for j, c in enumerate(n["choices"]):
                out += (",\n       " if j else "\n       ") + "{\"text\": " + q(c["text"]) + field("if", c.get("if", ""))
                out += field("do", "; ".join(c.get("do", []))) + f', "goto": {q(c.get("goto") or "end")}' + "}"
            out += "\n     ]"
        if n.get("call"):
            out += ",\n     \"call\": " + q(n["call"])
        if n.get("next"):
            out += ",\n     \"next\": " + q(n["next"])
        out += "}"
    out += "\n  ]\n}\n" if talk["nodes"] else "]\n}\n"
    return out


def convert(source: Path, out: Path, settings: dict, report):
    folder = source if source.is_dir() else source.parent
    if settings.get("whole", True) or source.is_dir():
        files = read_project(folder)
    else:
        files = {source.name: source.read_text(encoding="utf-8-sig", errors="replace")}
    if not files:
        raise ConvertError("в папке нет файлов .rpy")
    report(0.1, f"читаю {len(files)} файлов")
    project = Project(files)
    report(0.4, "собираю разговоры")
    talks = project.talks()
    if not talks:
        raise ConvertError("в сценарии нет ни одной метки (label), разговоров нет")
    written = []
    for i, (talk, data) in enumerate(sorted(talks.items())):
        path = out / f"{talk}.json"
        path.write_text(talk_json(data), encoding="utf-8")
        written.append(path)
        report(0.4 + 0.5 * (i + 1) / len(talks), talk)
    cast = out / "characters.cast.json"
    cast.write_text(json.dumps(project.cast_json(), ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    written.append(cast)
    system = [f for f in files if Path(f).name in SYSTEM_FILES or Project.talk_id(f) not in talks]
    settings_file = out / "novel.settings.json"
    settings_file.write_text(json.dumps(project.settings_json(system), ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    written.append(settings_file)
    notes = out / "Импорт Ren'Py.txt"
    notes.write_text(report_text(project, talks), encoding="utf-8")
    written.append(notes)
    return written
