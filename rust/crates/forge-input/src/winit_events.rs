//! The window's keyboard, mouse and focus events (winit) as [`Event`]s in SDL's codes, so a bind means the same key
//! as in the C++ version: the key's place (`PhysicalKey`), not the letter of the layout.

use crate::{Event, MOUSE_LEFT, MOUSE_MIDDLE, MOUSE_RIGHT, MOUSE_X1, MOUSE_X2};
use winit::event::{ElementState, MouseButton, WindowEvent};
use winit::keyboard::{KeyCode, PhysicalKey};

/// The event as input, or `None` for anything input does not need (the mouse moving, a key without a place).
pub fn event_of(e: &WindowEvent) -> Option<Event> {
    match e {
        WindowEvent::KeyboardInput { event, .. } => {
            let PhysicalKey::Code(code) = event.physical_key else { return None };
            Some(Event::Key {
                scancode: scancode_of(code)?,
                down: event.state == ElementState::Pressed,
                repeat: event.repeat,
            })
        }
        WindowEvent::MouseInput { state, button, .. } => {
            let button = match button {
                MouseButton::Left => MOUSE_LEFT,
                MouseButton::Right => MOUSE_RIGHT,
                MouseButton::Middle => MOUSE_MIDDLE,
                MouseButton::Back => MOUSE_X1,
                MouseButton::Forward => MOUSE_X2,
                MouseButton::Other(_) => return None,
            };
            Some(Event::Mouse { button, down: *state == ElementState::Pressed })
        }
        WindowEvent::Focused(false) => Some(Event::FocusLost),
        _ => None,
    }
}

/// The USB HID usage of the key, which is SDL's scancode.
pub fn scancode_of(code: KeyCode) -> Option<u16> {
    use KeyCode::*;
    let letters = [
        KeyA, KeyB, KeyC, KeyD, KeyE, KeyF, KeyG, KeyH, KeyI, KeyJ, KeyK, KeyL, KeyM, KeyN, KeyO, KeyP, KeyQ, KeyR,
        KeyS, KeyT, KeyU, KeyV, KeyW, KeyX, KeyY, KeyZ,
    ];
    if let Some(i) = letters.iter().position(|k| *k == code) {
        return Some(4 + i as u16);
    }
    let digits = [Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9, Digit0];
    if let Some(i) = digits.iter().position(|k| *k == code) {
        return Some(30 + i as u16);
    }
    let f = [F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12];
    if let Some(i) = f.iter().position(|k| *k == code) {
        return Some(58 + i as u16);
    }
    let f13 = [F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24];
    if let Some(i) = f13.iter().position(|k| *k == code) {
        return Some(104 + i as u16);
    }
    let pad = [Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8, Numpad9, Numpad0];
    if let Some(i) = pad.iter().position(|k| *k == code) {
        return Some(89 + i as u16);
    }
    Some(match code {
        Enter => 40,
        Escape => 41,
        Backspace => 42,
        Tab => 43,
        Space => 44,
        Minus => 45,
        Equal => 46,
        BracketLeft => 47,
        BracketRight => 48,
        Backslash => 49,
        Semicolon => 51,
        Quote => 52,
        Backquote => 53,
        Comma => 54,
        Period => 55,
        Slash => 56,
        CapsLock => 57,
        PrintScreen => 70,
        ScrollLock => 71,
        Pause => 72,
        Insert => 73,
        Home => 74,
        PageUp => 75,
        Delete => 76,
        End => 77,
        PageDown => 78,
        ArrowRight => 79,
        ArrowLeft => 80,
        ArrowDown => 81,
        ArrowUp => 82,
        NumLock => 83,
        NumpadDivide => 84,
        NumpadMultiply => 85,
        NumpadSubtract => 86,
        NumpadAdd => 87,
        NumpadEnter => 88,
        NumpadDecimal => 99,
        IntlBackslash => 100,
        ContextMenu => 101,
        Power => 102,
        NumpadEqual => 103,
        Help => 117,
        AudioVolumeMute => 127,
        AudioVolumeUp => 128,
        AudioVolumeDown => 129,
        NumpadComma => 133,
        IntlRo => 135,
        KanaMode => 136,
        IntlYen => 137,
        Convert => 138,
        NonConvert => 139,
        Lang1 => 144,
        Lang2 => 145,
        ControlLeft => 224,
        ShiftLeft => 225,
        AltLeft => 226,
        SuperLeft => 227,
        ControlRight => 228,
        ShiftRight => 229,
        AltRight => 230,
        SuperRight => 231,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{format, scancode_name};

    #[test]
    fn keys_are_named_as_sdl_names_them() {
        let named = [
            (KeyCode::KeyA, "A"),
            (KeyCode::KeyW, "W"),
            (KeyCode::Digit0, "0"),
            (KeyCode::Space, "Space"),
            (KeyCode::Enter, "Return"),
            (KeyCode::Escape, "Escape"),
            (KeyCode::ArrowLeft, "Left"),
            (KeyCode::ShiftLeft, "Left Shift"),
            (KeyCode::AltRight, "Right Alt"),
            (KeyCode::F5, "F5"),
            (KeyCode::F24, "F24"),
            (KeyCode::Numpad0, "Keypad 0"),
            (KeyCode::NumpadEnter, "Keypad Enter"),
            (KeyCode::Backquote, "`"),
        ];
        for (code, name) in named {
            assert_eq!(scancode_of(code).map(scancode_name), Some(name), "{code:?}");
        }
        assert_eq!(format(&crate::Bind::key(scancode_of(KeyCode::KeyK).unwrap())), "key:K");
    }
}
