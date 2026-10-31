//! The C++ version's tests of engine/input (tests/test_input.cpp), here: actions and their binds, the layers of
//! choices, input.json, clashes, held and pressed from events, gamepads, swallowed presses.

use forge_input::*;

fn defs() -> Vec<ActionDef> {
    vec![
        ActionDef::new("left", "Влево", &["key:A", "key:Left", "pad:dpleft", "pad:leftx-"]),
        ActionDef::new("jump", "Прыжок", &["key:Space", "key:W", "pad:south"]),
        ActionDef::new("talk", "Поговорить", &["key:E", "pad:west"]),
    ]
}

fn b(text: &str) -> Bind {
    parse(text).unwrap_or_else(|| panic!("{text}"))
}

fn sc(name: &str) -> u16 {
    scancode_from_name(name).unwrap()
}

fn key(name: &str, down: bool) -> Event {
    Event::Key { scancode: sc(name), down, repeat: false }
}

fn repeat(name: &str) -> Event {
    Event::Key { scancode: sc(name), down: true, repeat: true }
}

fn mouse(button: u8, down: bool) -> Event {
    Event::Mouse { button, down }
}

fn choice(action: &str, binds: &[&str]) -> Choice {
    Choice { action: action.into(), binds: binds.iter().map(|s| (*s).into()).collect() }
}

fn strings(v: &[&str]) -> Vec<String> {
    v.iter().map(|s| (*s).into()).collect()
}

#[test]
fn a_bind_is_read_and_written_back_with_words() {
    for t in [
        "key:Space",
        "key:A",
        "key:F5",
        "key:Escape",
        "key:Left",
        "key:Left Shift",
        "mouse:left",
        "mouse:x2",
        "pad:south",
        "pad:start",
        "pad:dpleft",
        "pad:leftx-",
        "pad:righty+",
        "pad:rt",
    ] {
        assert_eq!(format(&b(t)), t);
        assert_ne!(words(&b(t)), "?", "{t}");
    }
    assert_eq!(words(&b("key:Space")), "Пробел");
    assert_eq!(words(&b("key:Left")), "←");
    assert_eq!(words(&b("key:K")), "K");
    assert_eq!(words(&b("mouse:left")), "Левая кнопка мыши");
    assert_eq!(words(&b("pad:south")), "Геймпад: A (нижняя)");
    assert_eq!(words(&b("pad:leftx-")), "Геймпад: левый стик ←");
    assert_eq!(words(&b("pad:lt")), "Геймпад: левый курок");
    assert!(b("pad:lt").is_pad());
    assert!(!b("mouse:right").is_pad());
    for bad in ["", "Space", "key:", "key:Пробел", "key:NoSuchKey", "mouse:side", "pad:z", "pad:leftx", "joy:1"] {
        assert!(parse(bad).is_none(), "{bad}");
    }
    assert_eq!(b("key:W").code, 26); // the key's place (SDL_SCANCODE_W), whatever the layout
}

#[test]
fn defaults_then_the_authors_then_the_players() {
    let mut warnings = Vec::new();
    let a = resolve(&defs(), &[], &[], Some(&mut warnings));
    assert_eq!(a.len(), 3);
    assert!(warnings.is_empty());
    assert_eq!(a[1].id, "jump");
    assert_eq!(a[1].binds, vec![b("key:Space"), b("key:W"), b("pad:south")]);

    let author = vec![choice("jump", &["key:K", "pad:south"])];
    let a = resolve(&defs(), &author, &[], Some(&mut warnings));
    assert_eq!(a[1].binds, vec![b("key:K"), b("pad:south")]);
    assert_eq!(a[0].binds.len(), 4);

    let player = vec![choice("jump", &["key:L"])];
    let a = resolve(&defs(), &author, &player, Some(&mut warnings));
    assert_eq!(a[1].binds, vec![b("key:L")]);
    assert!(warnings.is_empty());

    let a = resolve(
        &defs(),
        &[choice("fly", &["key:F"]), choice("talk", &["key:Nope", "key:T", "key:T"])],
        &[],
        Some(&mut warnings),
    );
    assert_eq!(a[2].binds, vec![b("key:T")]);
    assert_eq!(
        warnings,
        strings(&[
            "клавиши автора: у «Поговорить» привязка «key:Nope» ничего не называет",
            "клавиши автора: действия «fly» нет в этой игре",
        ])
    );

    let a = resolve(&defs(), &[choice("talk", &[])], &[], None);
    assert!(a[2].binds.is_empty());
}

#[test]
fn choices_are_input_json_and_a_choice_equal_to_the_base_is_none() {
    let d = defs();
    let mut c = Choices::new();
    choose(&mut c, "jump", strings(&["key:K", "pad:south"]), &d[1].defaults);
    choose(&mut c, "talk", strings(&["key:E", "pad:west"]), &d[2].defaults);
    choose(&mut c, "left", strings(&["key:J"]), &d[0].defaults);
    assert_eq!(c.len(), 2);
    let text = write_choices(&c);
    assert_eq!(
        text,
        "{\n  \"actions\": [\n    {\"id\": \"jump\", \"binds\": [\"key:K\", \"pad:south\"]},\n    {\"id\": \"left\", \"binds\": [\"key:J\"]}\n  ]\n}\n"
    );
    let back = read_choices(&text, None).unwrap();
    assert_eq!(back, c);
    choose(&mut c, "jump", d[1].defaults.clone(), &d[1].defaults);
    assert_eq!(c.len(), 1);
    assert_eq!(c[0].action, "left");
    assert_eq!(write_choices(&[]), "{\n  \"actions\": []\n}\n");

    assert_eq!(read_choices("{\"actions\": 3}", None), Err("нет списка «actions»".into()));
    assert_eq!(read_choices("not json", None), Err("это не JSON".into()));
    let mut warnings = Vec::new();
    let back = read_choices(
        "{\"actions\": [{\"id\": 5}, {\"id\": \"jump\", \"binds\": [\"key:K\"]}, {\"id\": \"jump\", \"binds\": []}]}",
        Some(&mut warnings),
    )
    .unwrap();
    assert_eq!(back, vec![choice("jump", &["key:K"])]);
    assert_eq!(
        warnings,
        strings(&["действие 1: нужны «id» и список «binds»", "действие «jump» дважды: берётся первое"])
    );
}

#[test]
fn one_bind_on_two_actions_is_said_in_words() {
    let a = resolve(&defs(), &[choice("talk", &["key:Space", "key:E"])], &[], None);
    let c = clashes(&a);
    assert_eq!(c.len(), 1);
    assert_eq!(clash_words(&c[0]), "Пробел — у «Прыжок» и «Поговорить»");
    assert!(clashes(&resolve(&defs(), &[], &[], None)).is_empty());
    assert!(clashes(&resolve(&shell_actions(), &[], &[], None)).is_empty());
}

#[test]
fn held_and_pressed_come_from_events() {
    let mut i = Input::default();
    i.set(resolve(&defs(), &[choice("jump", &["key:K", "mouse:right"])], &[], None));
    assert!(!i.down("jump"));
    i.handle_event(&key("Space", true)); // the module's key, not the author's
    assert!(!i.down("jump") && !i.pressed("jump"));
    i.handle_event(&key("K", true));
    assert!(i.down("jump") && i.pressed("jump"));
    assert!(i.presses(&key("K", true), "jump"));
    assert!(!i.presses(&repeat("K"), "jump"));
    i.end_frame();
    assert!(i.down("jump") && !i.pressed("jump"));
    i.handle_event(&repeat("K"));
    assert!(!i.pressed("jump"));
    i.handle_event(&key("K", false));
    assert!(!i.down("jump"));
    // Down and up within one frame: pressed all the same, not held.
    i.end_frame();
    i.handle_event(&key("K", true));
    i.handle_event(&key("K", false));
    assert!(i.pressed("jump") && !i.down("jump"));
    i.end_frame();
    i.handle_event(&mouse(MOUSE_RIGHT, true));
    assert!(i.down("jump"));
    i.handle_event(&mouse(MOUSE_RIGHT, false));
    assert!(!i.down("jump"));
    i.handle_event(&key("A", true));
    assert!(i.down("left"));
    i.handle_event(&Event::FocusLost);
    assert!(!i.down("left"));
    assert_eq!(i.first_words("jump"), "K");
    assert_eq!(i.first_words("left"), "A");
    assert_eq!(i.first_words("nope"), "");
    assert!(!i.down("nope"));

    assert_eq!(bind_of(&key("K", true)), Some(b("key:K")));
    assert_eq!(bind_of(&key("K", false)), None);
    assert_eq!(bind_of(&repeat("K")), None);
    assert_eq!(bind_of(&mouse(MOUSE_LEFT, true)), Some(b("mouse:left")));
    assert_eq!(bind_of(&Event::PadAxis { pad: 1, axis: pad::LEFTX, value: -20000 }), Some(b("pad:leftx-")));
    assert_eq!(bind_of(&Event::PadAxis { pad: 1, axis: pad::LEFTX, value: -10000 }), None);
}

fn button(button: u8, down: bool) -> Event {
    Event::PadButton { pad: 7, button, down }
}

fn axis(axis: u8, value: i16) -> Event {
    Event::PadAxis { pad: 7, axis, value }
}

#[test]
fn a_gamepad_comes_is_read_and_goes() {
    let mut i = Input::default();
    i.set(resolve(&defs(), &[], &[], None));
    i.handle_event(&Event::PadAdded(7));
    assert_eq!(i.gamepads(), 1);
    i.handle_event(&button(pad::SOUTH, true));
    assert!(i.down("jump") && i.pressed("jump"));
    i.end_frame();
    i.handle_event(&button(pad::SOUTH, false));
    assert!(!i.down("jump"));
    i.handle_event(&axis(pad::LEFTX, -8000));
    assert!(!i.down("left"));
    i.handle_event(&axis(pad::LEFTX, -30000));
    assert!(i.down("left") && i.pressed("left"));
    i.end_frame();
    i.handle_event(&axis(pad::LEFTX, -31000));
    assert!(!i.pressed("left"));
    i.handle_event(&axis(pad::LEFTX, 0));
    assert!(!i.down("left"));
    assert_eq!(i.first_words("jump"), "Пробел");
    i.handle_event(&button(pad::WEST, true));
    assert!(i.down("talk"));
    i.handle_event(&Event::PadRemoved(7));
    assert_eq!(i.gamepads(), 0);
    assert!(!i.down("talk"));
}

#[test]
fn a_swallowed_press_is_no_actions_until_let_go() {
    let mut i = Input::default();
    i.set(resolve(&defs(), &[], &[], None));
    i.handle_event(&key("A", true));
    let e = key("E", true);
    i.handle_event(&e);
    i.swallow(&e);
    assert!(!i.pressed("talk") && !i.down("talk"));
    assert!(i.pressed("left") && i.down("left"));
    i.end_frame();
    i.handle_event(&repeat("E"));
    assert!(!i.pressed("talk") && !i.down("talk"));
    i.handle_event(&key("E", false));
    assert!(!i.down("talk"));
    i.handle_event(&e);
    assert!(i.pressed("talk") && i.down("talk"));
    i.handle_event(&key("E", false));
    i.end_frame();
    // Space swallowed, W of the same action held: the action is held by W.
    let space = key("Space", true);
    i.handle_event(&space);
    i.swallow(&space);
    assert!(!i.down("jump"));
    i.handle_event(&key("W", true));
    assert!(i.down("jump") && i.pressed("jump"));
    i.handle_event(&key("W", false));
    assert!(!i.down("jump"));
    i.end_frame();
    i.handle_event(&Event::FocusLost);
    i.handle_event(&space);
    assert!(i.pressed("jump") && i.down("jump"));
    i.release_all();
    // A mouse button, swallowed, until it is let go; a new game forgets it too.
    i.set(resolve(&defs(), &[choice("jump", &["mouse:left"])], &[], None));
    let click = mouse(MOUSE_LEFT, true);
    i.handle_event(&click);
    i.swallow(&click);
    assert!(!i.pressed("jump") && !i.down("jump"));
    i.handle_event(&mouse(MOUSE_LEFT, false));
    i.end_frame();
    i.handle_event(&click);
    assert!(i.down("jump"));
    i.swallow(&click);
    assert!(!i.down("jump"));
    i.release_all();
    i.handle_event(&click);
    assert!(i.down("jump"));
    i.swallow(&key("K", false));
    assert!(i.down("jump"));
}

#[test]
fn a_swallowed_gamepad_button_and_stick() {
    let mut i = Input::default();
    i.set(resolve(&defs(), &[], &[], None));
    i.handle_event(&Event::PadAdded(7));
    let south = button(pad::SOUTH, true);
    i.handle_event(&south);
    i.swallow(&south);
    assert!(!i.pressed("jump") && !i.down("jump"));
    i.end_frame();
    i.handle_event(&button(pad::SOUTH, false));
    i.handle_event(&south);
    assert!(i.pressed("jump") && i.down("jump"));
    i.handle_event(&button(pad::SOUTH, false));
    i.end_frame();
    let stick = axis(pad::LEFTX, -30000);
    i.handle_event(&stick);
    i.swallow(&stick);
    assert!(!i.pressed("left") && !i.down("left"));
    i.end_frame();
    i.handle_event(&axis(pad::LEFTX, -31000));
    assert!(!i.down("left"));
    i.handle_event(&axis(pad::LEFTX, 0));
    i.handle_event(&axis(pad::LEFTX, -30000));
    assert!(i.pressed("left") && i.down("left"));
}
