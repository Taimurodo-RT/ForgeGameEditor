//! What the player does, by name (the editor's tab «Ввод», editors/ввод-модель.md), ported from engine/input: «Прыжок»,
//! «Пауза». A game asks `down("jump")` and `pressed("talk")`, not which key is held; which keys, mouse buttons and
//! gamepad buttons make an action is the module's default, then the author's (the game's input.json), then the
//! player's («Настройки», settings.json).
//!
//! ```ignore
//! let mut input = Input::default();
//! input.set(resolve(&defs, &author, &player, None));
//! input.handle_event(&e);           // every event, before the UI takes it
//! if input.down("left") { … }       // held
//! if input.pressed("talk") { … }    // went down since the last end_frame()
//! input.end_frame();
//! ```
//!
//! A bind is written as text, the same as the C++ version: `key:Space` (the key's place on the keyboard as SDL names
//! it, not the letter of the layout), `mouse:left`, `pad:south`, `pad:leftx-` (a stick or a trigger past half way).
//! Codes are SDL's (scancodes are USB HID usages), so a bind means the same in both versions; tests/data/input_binds.txt
//! is checked by both. Events come in as [`Event`]; `winit` and `gilrs` (features of the same names) turn the window's
//! and the gamepads' events into them.

#[cfg(feature = "gilrs")]
pub mod gilrs_events;
mod scancodes;
#[cfg(feature = "winit")]
pub mod winit_events;

use scancodes::SCANCODE_NAMES;

/// A stick or a trigger counts as pressed past half way.
const HALF: i32 = 16384;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Device {
    Key,
    Mouse,
    Pad,
    PadAxis,
}

/// One key, mouse button, gamepad button, or stick or trigger direction.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Bind {
    pub device: Device,
    /// SDL's: the scancode, `MOUSE_LEFT`…, the gamepad button or axis.
    pub code: i32,
    /// A stick: -1 or +1; a trigger: +1; 0 otherwise.
    pub dir: i8,
}

impl Bind {
    pub const fn key(scancode: u16) -> Bind {
        Bind { device: Device::Key, code: scancode as i32, dir: 0 }
    }
    /// Whether it is a gamepad's (else the player's keyboard or mouse).
    pub fn is_pad(&self) -> bool {
        matches!(self.device, Device::Pad | Device::PadAxis)
    }
}

// SDL's mouse buttons, gamepad buttons and axes.
pub const MOUSE_LEFT: u8 = 1;
pub const MOUSE_MIDDLE: u8 = 2;
pub const MOUSE_RIGHT: u8 = 3;
pub const MOUSE_X1: u8 = 4;
pub const MOUSE_X2: u8 = 5;

pub mod pad {
    pub const SOUTH: u8 = 0;
    pub const EAST: u8 = 1;
    pub const WEST: u8 = 2;
    pub const NORTH: u8 = 3;
    pub const BACK: u8 = 4;
    pub const GUIDE: u8 = 5;
    pub const START: u8 = 6;
    pub const LEFT_STICK: u8 = 7;
    pub const RIGHT_STICK: u8 = 8;
    pub const LEFT_SHOULDER: u8 = 9;
    pub const RIGHT_SHOULDER: u8 = 10;
    pub const DPAD_UP: u8 = 11;
    pub const DPAD_DOWN: u8 = 12;
    pub const DPAD_LEFT: u8 = 13;
    pub const DPAD_RIGHT: u8 = 14;

    pub const LEFTX: u8 = 0;
    pub const LEFTY: u8 = 1;
    pub const RIGHTX: u8 = 2;
    pub const RIGHTY: u8 = 3;
    pub const LEFT_TRIGGER: u8 = 4;
    pub const RIGHT_TRIGGER: u8 = 5;
}

const MOUSE: &[(&str, u8, &str)] = &[
    ("left", MOUSE_LEFT, "Левая кнопка мыши"),
    ("right", MOUSE_RIGHT, "Правая кнопка мыши"),
    ("middle", MOUSE_MIDDLE, "Средняя кнопка мыши"),
    ("x1", MOUSE_X1, "Кнопка мыши 4"),
    ("x2", MOUSE_X2, "Кнопка мыши 5"),
];

const PAD_BUTTONS: &[(&str, u8, &str)] = &[
    ("south", pad::SOUTH, "Геймпад: A (нижняя)"),
    ("east", pad::EAST, "Геймпад: B (правая)"),
    ("west", pad::WEST, "Геймпад: X (левая)"),
    ("north", pad::NORTH, "Геймпад: Y (верхняя)"),
    ("back", pad::BACK, "Геймпад: Back"),
    ("guide", pad::GUIDE, "Геймпад: Guide"),
    ("start", pad::START, "Геймпад: Start"),
    ("ls", pad::LEFT_STICK, "Геймпад: левый стик (нажатие)"),
    ("rs", pad::RIGHT_STICK, "Геймпад: правый стик (нажатие)"),
    ("lb", pad::LEFT_SHOULDER, "Геймпад: левый бампер"),
    ("rb", pad::RIGHT_SHOULDER, "Геймпад: правый бампер"),
    ("dpup", pad::DPAD_UP, "Геймпад: крестовина ↑"),
    ("dpdown", pad::DPAD_DOWN, "Геймпад: крестовина ↓"),
    ("dpleft", pad::DPAD_LEFT, "Геймпад: крестовина ←"),
    ("dpright", pad::DPAD_RIGHT, "Геймпад: крестовина →"),
];

const PAD_AXES: &[(&str, u8, i8, &str)] = &[
    ("leftx-", pad::LEFTX, -1, "Геймпад: левый стик ←"),
    ("leftx+", pad::LEFTX, 1, "Геймпад: левый стик →"),
    ("lefty-", pad::LEFTY, -1, "Геймпад: левый стик ↑"),
    ("lefty+", pad::LEFTY, 1, "Геймпад: левый стик ↓"),
    ("rightx-", pad::RIGHTX, -1, "Геймпад: правый стик ←"),
    ("rightx+", pad::RIGHTX, 1, "Геймпад: правый стик →"),
    ("righty-", pad::RIGHTY, -1, "Геймпад: правый стик ↑"),
    ("righty+", pad::RIGHTY, 1, "Геймпад: правый стик ↓"),
    ("lt", pad::LEFT_TRIGGER, 1, "Геймпад: левый курок"),
    ("rt", pad::RIGHT_TRIGGER, 1, "Геймпад: правый курок"),
];

/// Keys whose SDL names are not what a player of a Russian game calls them.
const KEY_WORDS: &[(u16, &str)] = &[
    (44, "Пробел"),
    (40, "Enter"),
    (41, "Esc"),
    (80, "←"),
    (79, "→"),
    (82, "↑"),
    (81, "↓"),
    (225, "Левый Shift"),
    (229, "Правый Shift"),
    (224, "Левый Ctrl"),
    (228, "Правый Ctrl"),
    (226, "Левый Alt"),
    (230, "Правый Alt"),
    (88, "Enter (цифровой блок)"),
];

/// SDL's name of a scancode ("" for none).
pub fn scancode_name(scancode: u16) -> &'static str {
    SCANCODE_NAMES.iter().find(|(c, _)| *c == scancode).map_or("", |(_, n)| n)
}

/// The scancode SDL names so, ignoring ASCII case as `SDL_GetScancodeFromName` does.
pub fn scancode_from_name(name: &str) -> Option<u16> {
    SCANCODE_NAMES.iter().find(|(_, n)| n.eq_ignore_ascii_case(name)).map(|(c, _)| *c)
}

/// `key:Space` … `None` when the text names nothing.
pub fn parse(text: &str) -> Option<Bind> {
    let (kind, name) = text.split_once(':')?;
    if name.is_empty() {
        return None;
    }
    match kind {
        "key" => scancode_from_name(name).map(Bind::key),
        "mouse" => MOUSE.iter().find(|m| m.0 == name).map(|m| Bind { device: Device::Mouse, code: m.1 as i32, dir: 0 }),
        "pad" => PAD_BUTTONS
            .iter()
            .find(|b| b.0 == name)
            .map(|b| Bind { device: Device::Pad, code: b.1 as i32, dir: 0 })
            .or_else(|| {
                PAD_AXES.iter().find(|a| a.0 == name).map(|a| Bind {
                    device: Device::PadAxis,
                    code: a.1 as i32,
                    dir: a.2,
                })
            }),
        _ => None,
    }
}

/// The bind as text; "" when it names nothing.
pub fn format(b: &Bind) -> String {
    match b.device {
        Device::Key => match u16::try_from(b.code).map(scancode_name) {
            Ok(name) if !name.is_empty() => format!("key:{name}"),
            _ => String::new(),
        },
        Device::Mouse => {
            MOUSE.iter().find(|m| m.1 as i32 == b.code).map_or(String::new(), |m| format!("mouse:{}", m.0))
        }
        Device::Pad => {
            PAD_BUTTONS.iter().find(|p| p.1 as i32 == b.code).map_or(String::new(), |p| format!("pad:{}", p.0))
        }
        Device::PadAxis => PAD_AXES
            .iter()
            .find(|a| a.1 as i32 == b.code && a.2 == b.dir)
            .map_or(String::new(), |a| format!("pad:{}", a.0)),
    }
}

/// In the player's words: «Пробел», «←», «Левая кнопка мыши», «Геймпад: A (нижняя)», «Геймпад: левый стик ←».
pub fn words(b: &Bind) -> String {
    let found = match b.device {
        Device::Key => {
            let Ok(code) = u16::try_from(b.code) else { return "?".into() };
            if let Some((_, w)) = KEY_WORDS.iter().find(|(c, _)| *c == code) {
                return (*w).into();
            }
            let name = scancode_name(code);
            return if name.is_empty() { "?".into() } else { name.into() };
        }
        Device::Mouse => MOUSE.iter().find(|m| m.1 as i32 == b.code).map(|m| m.2),
        Device::Pad => PAD_BUTTONS.iter().find(|p| p.1 as i32 == b.code).map(|p| p.2),
        Device::PadAxis => PAD_AXES.iter().find(|a| a.1 as i32 == b.code && a.2 == b.dir).map(|a| a.3),
    };
    found.unwrap_or("?").into()
}

/// Every mouse button, gamepad button, stick direction and trigger, in that order: what an author picks from a list
/// without a gamepad at hand.
pub fn mouse_and_pad_binds() -> Vec<Bind> {
    let mut out: Vec<Bind> = MOUSE.iter().map(|m| Bind { device: Device::Mouse, code: m.1 as i32, dir: 0 }).collect();
    out.extend(PAD_BUTTONS.iter().map(|p| Bind { device: Device::Pad, code: p.1 as i32, dir: 0 }));
    out.extend(PAD_AXES.iter().map(|a| Bind { device: Device::PadAxis, code: a.1 as i32, dir: a.2 }));
    out
}

/// An action a module or the shell has, with its default binds (as text).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ActionDef {
    pub id: String,
    pub words: String,
    pub defaults: Vec<String>,
}

impl ActionDef {
    pub fn new(id: &str, words: &str, defaults: &[&str]) -> ActionDef {
        ActionDef { id: id.into(), words: words.into(), defaults: defaults.iter().map(|s| (*s).into()).collect() }
    }
}

/// The shell's own, in every game: pause, journal, quicksave, quickload.
pub fn shell_actions() -> Vec<ActionDef> {
    vec![
        ActionDef::new("pause", "Пауза", &["key:Escape", "pad:start"]),
        ActionDef::new("journal", "Журнал", &["key:J", "pad:back"]),
        ActionDef::new("quicksave", "Быстрое сохранение", &["key:F5"]),
        ActionDef::new("quickload", "Быстрая загрузка", &["key:F9"]),
    ]
}

/// An action's binds as someone chose them (the author's input.json, the player's settings.json "keys").
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Choice {
    pub action: String,
    pub binds: Vec<String>,
}

pub type Choices = Vec<Choice>;

/// input.json: `{"actions": [{"id": "jump", "binds": ["key:K"]}]}`. `Err` and why when it is not that (the file stays
/// unread); a bad entry in it is left out with a warning.
pub fn read_choices(json: &str, warnings: Option<&mut Vec<String>>) -> Result<Choices, String> {
    let doc: serde_json::Value = serde_json::from_str(json).map_err(|_| "это не JSON".to_string())?;
    let Some(list) = doc.get("actions").and_then(|a| a.as_array()) else {
        return Err("нет списка «actions»".into());
    };
    let mut said = Vec::new();
    let mut out: Choices = Vec::new();
    for (i, item) in list.iter().enumerate() {
        let (Some(id), Some(binds)) =
            (item.get("id").and_then(|v| v.as_str()), item.get("binds").and_then(|v| v.as_array()))
        else {
            said.push(format!("действие {}: нужны «id» и список «binds»", i + 1));
            continue;
        };
        if find(&out, id).is_some() {
            said.push(format!("действие «{id}» дважды: берётся первое"));
            continue;
        }
        let binds = binds.iter().filter_map(|b| b.as_str().map(str::to_owned)).collect();
        out.push(Choice { action: id.into(), binds });
    }
    if let Some(w) = warnings {
        w.extend(said);
    }
    Ok(out)
}

fn json_text(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        if c == '"' || c == '\\' {
            out.push('\\');
        }
        if (c as u32) < 0x20 {
            continue;
        }
        out.push(c);
    }
    out.push('"');
    out
}

/// The text of input.json, byte for byte as the C++ version writes it.
pub fn write_choices(choices: &[Choice]) -> String {
    let mut out = String::from("{\n  \"actions\": [");
    for (i, c) in choices.iter().enumerate() {
        out += if i > 0 { ",\n    " } else { "\n    " };
        out += &format!("{{\"id\": {}, \"binds\": [", json_text(&c.action));
        let binds: Vec<String> = c.binds.iter().map(|b| json_text(b)).collect();
        out += &binds.join(", ");
        out += "]}";
    }
    out += if choices.is_empty() { "]\n}\n" } else { "\n  ]\n}\n" };
    out
}

/// The choice for an action.
pub fn find<'a>(choices: &'a [Choice], action: &str) -> Option<&'a Choice> {
    choices.iter().find(|c| c.action == action)
}

/// Sets the choice for an action, or removes it when `binds` are what the action has without it (`base`: the module's
/// defaults for the author, the author's binds for the player); keeps the others in order.
pub fn choose(choices: &mut Choices, action: &str, binds: Vec<String>, base: &[String]) {
    let at = choices.iter().position(|c| c.action == action);
    if binds == base {
        if let Some(i) = at {
            choices.remove(i);
        }
        return;
    }
    match at {
        Some(i) => choices[i].binds = binds,
        None => choices.push(Choice { action: action.into(), binds }),
    }
}

/// An action as it plays: its binds after the defaults, the author's and the player's choices.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Action {
    pub id: String,
    pub words: String,
    pub binds: Vec<Bind>,
}

fn parse_all(texts: &[String], who: &str, action: &str, warnings: &mut Option<&mut Vec<String>>) -> Vec<Bind> {
    let mut out = Vec::new();
    for t in texts {
        match parse(t) {
            Some(b) if !out.contains(&b) => out.push(b),
            Some(_) => {}
            None => {
                if let Some(w) = warnings {
                    w.push(format!("{who}: у «{action}» привязка «{t}» ничего не называет"));
                }
            }
        }
    }
    out
}

/// `defs` in their order; each layer replaces an action's binds. A choice for an action `defs` have not and a bind
/// that names nothing are left out, each with a warning in words.
pub fn resolve(
    defs: &[ActionDef],
    author: &[Choice],
    player: &[Choice],
    mut warnings: Option<&mut Vec<String>>,
) -> Vec<Action> {
    let mut out = Vec::new();
    for d in defs {
        let mut binds = parse_all(&d.defaults, "клавиши модуля", &d.words, &mut warnings);
        if let Some(c) = find(author, &d.id) {
            binds = parse_all(&c.binds, "клавиши автора", &d.words, &mut warnings);
        }
        if let Some(c) = find(player, &d.id) {
            binds = parse_all(&c.binds, "клавиши игрока", &d.words, &mut warnings);
        }
        out.push(Action { id: d.id.clone(), words: d.words.clone(), binds });
    }
    if let Some(w) = warnings {
        let known = |id: &str| defs.iter().any(|d| d.id == id);
        for c in author.iter().filter(|c| !known(&c.action)) {
            w.push(format!("клавиши автора: действия «{}» нет в этой игре", c.action));
        }
        for c in player.iter().filter(|c| !known(&c.action)) {
            w.push(format!("клавиши игрока: действия «{}» нет в этой игре", c.action));
        }
    }
    out
}

/// One bind on two actions.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Clash {
    pub bind: Bind,
    /// The actions' words, in their order.
    pub a: String,
    pub b: String,
}

pub fn clashes(actions: &[Action]) -> Vec<Clash> {
    let mut out = Vec::new();
    for (i, x) in actions.iter().enumerate() {
        for y in &actions[i + 1..] {
            for b in x.binds.iter().filter(|b| y.binds.contains(b)) {
                out.push(Clash { bind: *b, a: x.words.clone(), b: y.words.clone() });
            }
        }
    }
    out
}

/// «Пробел — у «Прыжок» и «Поговорить»».
pub fn clash_words(c: &Clash) -> String {
    format!("{} — у «{}» и «{}»", words(&c.bind), c.a, c.b)
}

/// What the window and the gamepads tell, in SDL's codes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Key {
        scancode: u16,
        down: bool,
        repeat: bool,
    },
    Mouse {
        button: u8,
        down: bool,
    },
    /// The window lost the keyboard: every key and button is let go.
    FocusLost,
    PadAdded(u32),
    PadRemoved(u32),
    PadButton {
        pad: u32,
        button: u8,
        down: bool,
    },
    /// SDL's range: -32768..32767 for sticks (down is +), 0..32767 for triggers.
    PadAxis {
        pad: u32,
        axis: u8,
        value: i16,
    },
}

/// The bind an event is: a key, a mouse button or a gamepad button going down, a stick or a trigger past half way.
/// None for anything else (a repeat, a key going up).
pub fn bind_of(e: &Event) -> Option<Bind> {
    match *e {
        Event::Key { scancode, down: true, repeat: false } if scancode != 0 => Some(Bind::key(scancode)),
        Event::Mouse { button, down: true } => Some(Bind { device: Device::Mouse, code: button as i32, dir: 0 }),
        Event::PadButton { button, down: true, .. } => Some(Bind { device: Device::Pad, code: button as i32, dir: 0 }),
        Event::PadAxis { axis, value, .. } => {
            let v = value as i32;
            if v.abs() < HALF {
                return None;
            }
            if axis == pad::LEFT_TRIGGER || axis == pad::RIGHT_TRIGGER {
                return (v > 0).then_some(Bind { device: Device::PadAxis, code: axis as i32, dir: 1 });
            }
            Some(Bind { device: Device::PadAxis, code: axis as i32, dir: if v < 0 { -1 } else { 1 } })
        }
        _ => None,
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct Pad {
    buttons: u32,   // bit per gamepad button
    axes: [i16; 8], // by axis
}

/// What is held and what went down, by action.
#[derive(Debug, Default)]
pub struct Input {
    actions: Vec<Action>,
    keys: Vec<u16>,                             // scancodes held
    mouse: u32,                                 // bit per mouse button
    pads: std::collections::BTreeMap<u32, Pad>, // by the gamepad's id
    went_down: Vec<Bind>,
    swallowed: Vec<Bind>, // held, but no action's until let go
}

impl Input {
    pub fn set(&mut self, actions: Vec<Action>) {
        self.actions = actions;
    }

    pub fn actions(&self) -> &[Action] {
        &self.actions
    }

    pub fn action(&self, id: &str) -> Option<&Action> {
        self.actions.iter().find(|a| a.id == id)
    }

    /// Every event: what is held, gamepads connected and gone, the window losing the keyboard (all let go).
    pub fn handle_event(&mut self, e: &Event) {
        match *e {
            Event::Key { scancode, down: true, repeat: false } if scancode != 0 => {
                if !self.keys.contains(&scancode) {
                    self.keys.push(scancode);
                }
                self.went_down.push(Bind::key(scancode));
            }
            Event::Key { scancode, down: false, .. } => {
                self.keys.retain(|k| *k != scancode);
                self.swallowed.retain(|b| *b != Bind::key(scancode));
            }
            Event::Mouse { button, down } => {
                let b = Bind { device: Device::Mouse, code: button as i32, dir: 0 };
                if down {
                    if button < 32 {
                        self.mouse |= 1 << button;
                    }
                    self.went_down.push(b);
                } else {
                    if button < 32 {
                        self.mouse &= !(1 << button);
                    }
                    self.swallowed.retain(|s| *s != b);
                }
            }
            Event::FocusLost => {
                self.keys.clear();
                self.mouse = 0;
                self.swallowed.clear();
            }
            Event::PadAdded(id) => {
                self.pads.entry(id).or_default();
            }
            Event::PadRemoved(id) => {
                self.pads.remove(&id);
            }
            Event::PadButton { pad, button, down } => {
                if let Some(p) = self.pads.get_mut(&pad)
                    && button < 32
                {
                    let b = Bind { device: Device::Pad, code: button as i32, dir: 0 };
                    if down {
                        p.buttons |= 1 << button;
                        self.went_down.push(b);
                    } else {
                        p.buttons &= !(1 << button);
                        self.swallowed.retain(|s| *s != b);
                    }
                }
            }
            Event::PadAxis { pad, axis, value } => {
                if let Some(p) = self.pads.get_mut(&pad)
                    && axis < 8
                {
                    let (before, now) = (p.axes[axis as usize] as i32, value as i32);
                    p.axes[axis as usize] = value;
                    for dir in [-1i8, 1] {
                        let b = Bind { device: Device::PadAxis, code: axis as i32, dir };
                        if before * (dir as i32) < HALF && now * (dir as i32) >= HALF {
                            self.went_down.push(b);
                        }
                        if now * (dir as i32) < HALF {
                            self.swallowed.retain(|s| *s != b);
                        }
                    }
                }
            }
            _ => {}
        }
    }

    /// The bind an event pressed as `handle_event` saw it: a stick only when it crossed half way just now.
    fn pressed_by(&self, e: &Event) -> Option<Bind> {
        if let Event::PadAxis { axis, .. } = *e {
            return self.went_down.last().filter(|b| b.device == Device::PadAxis && b.code == axis as i32).copied();
        }
        bind_of(e)
    }

    /// Whether this event presses the action (going down, not a repeat).
    pub fn presses(&self, e: &Event, action: &str) -> bool {
        let Some(a) = self.action(action) else { return false };
        self.pressed_by(e).is_some_and(|b| a.binds.contains(&b))
    }

    fn held(&self, b: &Bind) -> bool {
        if self.swallowed.contains(b) {
            return false;
        }
        match b.device {
            Device::Key => u16::try_from(b.code).is_ok_and(|c| self.keys.contains(&c)),
            Device::Mouse => (0..32).contains(&b.code) && self.mouse >> b.code & 1 != 0,
            Device::Pad => (0..32).contains(&b.code) && self.pads.values().any(|p| p.buttons >> b.code & 1 != 0),
            Device::PadAxis => {
                (0..8).contains(&b.code)
                    && self.pads.values().any(|p| p.axes[b.code as usize] as i32 * b.dir as i32 >= HALF)
            }
        }
    }

    pub fn down(&self, action: &str) -> bool {
        self.action(action).is_some_and(|a| a.binds.iter().any(|b| self.held(b)))
    }

    pub fn pressed(&self, action: &str) -> bool {
        self.action(action).is_some_and(|a| self.went_down.iter().any(|b| a.binds.contains(b)))
    }

    pub fn end_frame(&mut self) {
        self.went_down.clear();
    }

    /// Nothing held (a new game, a lost window).
    pub fn release_all(&mut self) {
        self.keys.clear();
        self.mouse = 0;
        for p in self.pads.values_mut() {
            *p = Pad::default();
        }
        self.went_down.clear();
        self.swallowed.clear();
    }

    /// The button this event pressed is neither held nor pressed for any action until it is let go: a press that
    /// moves a scene on is the scene's, not the world's (part 6г3 of docs/models/сцены-модель.md).
    pub fn swallow(&mut self, e: &Event) {
        let Some(b) = self.pressed_by(e) else { return };
        self.went_down.retain(|w| *w != b);
        if !self.swallowed.contains(&b) {
            self.swallowed.push(b);
        }
    }

    pub fn gamepads(&self) -> usize {
        self.pads.len()
    }

    /// The action's first keyboard or mouse bind in words, or its first gamepad bind; "" when it has none.
    pub fn first_words(&self, action: &str) -> String {
        let Some(a) = self.action(action) else { return String::new() };
        a.binds.iter().find(|b| !b.is_pad()).or(a.binds.first()).map_or(String::new(), words)
    }
}
