//! Gamepads (gilrs) as [`Event`]s in SDL's codes: the same buttons and axes as `pad:` binds of the C++ version. gilrs
//! calls the bumpers `LeftTrigger`/`RightTrigger` and the analog triggers `LeftTrigger2`/`RightTrigger2`; sticks point
//! up with +, SDL's with −, so Y is turned over.

use crate::{Event, pad};
use gilrs::{Axis, Button, EventType};

/// The gamepad event as input, or `None` for anything input does not need.
pub fn event_of(e: &gilrs::Event) -> Option<Event> {
    let id = usize::from(e.id) as u32;
    match e.event {
        EventType::Connected => Some(Event::PadAdded(id)),
        EventType::Disconnected => Some(Event::PadRemoved(id)),
        EventType::ButtonPressed(b, _) => button_of(b).map(|button| Event::PadButton { pad: id, button, down: true }),
        EventType::ButtonReleased(b, _) => button_of(b).map(|button| Event::PadButton { pad: id, button, down: false }),
        EventType::ButtonChanged(Button::LeftTrigger2, v, _) => Some(trigger(id, pad::LEFT_TRIGGER, v)),
        EventType::ButtonChanged(Button::RightTrigger2, v, _) => Some(trigger(id, pad::RIGHT_TRIGGER, v)),
        EventType::AxisChanged(axis, v, _) => {
            let (axis, value) = match axis {
                Axis::LeftStickX => (pad::LEFTX, v),
                Axis::LeftStickY => (pad::LEFTY, -v),
                Axis::RightStickX => (pad::RIGHTX, v),
                Axis::RightStickY => (pad::RIGHTY, -v),
                _ => return None,
            };
            Some(Event::PadAxis { pad: id, axis, value: (value.clamp(-1.0, 1.0) * 32767.0) as i16 })
        }
        _ => None,
    }
}

fn trigger(id: u32, axis: u8, v: f32) -> Event {
    Event::PadAxis { pad: id, axis, value: (v.clamp(0.0, 1.0) * 32767.0) as i16 }
}

/// SDL's button, or `None` for the analog triggers (axes) and buttons SDL's gamepads have not.
pub fn button_of(b: Button) -> Option<u8> {
    Some(match b {
        Button::South => pad::SOUTH,
        Button::East => pad::EAST,
        Button::West => pad::WEST,
        Button::North => pad::NORTH,
        Button::Select => pad::BACK,
        Button::Mode => pad::GUIDE,
        Button::Start => pad::START,
        Button::LeftThumb => pad::LEFT_STICK,
        Button::RightThumb => pad::RIGHT_STICK,
        Button::LeftTrigger => pad::LEFT_SHOULDER,
        Button::RightTrigger => pad::RIGHT_SHOULDER,
        Button::DPadUp => pad::DPAD_UP,
        Button::DPadDown => pad::DPAD_DOWN,
        Button::DPadLeft => pad::DPAD_LEFT,
        Button::DPadRight => pad::DPAD_RIGHT,
        _ => return None,
    })
}
