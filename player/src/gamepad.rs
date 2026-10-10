//! Gamepad input through gilrs (optional `gamepad` feature).

use crate::session::Shared;
use gilrs::{Axis, Button, EventType, Gilrs};
use std::sync::atomic::Ordering;
use vibrt::pad;

pub struct Pads {
    gilrs: Option<Gilrs>,
    held: u16,
    stick: u16,
}

impl Pads {
    pub fn new() -> Pads {
        Pads { gilrs: Gilrs::new().ok(), held: 0, stick: 0 }
    }

    fn bit(b: Button) -> u16 {
        match b {
            Button::South => pad::CROSS,
            Button::East => pad::CIRCLE,
            Button::West => pad::SQUARE,
            Button::North => pad::TRIANGLE,
            Button::LeftTrigger => pad::L1,
            Button::RightTrigger => pad::R1,
            Button::LeftTrigger2 => pad::L2,
            Button::RightTrigger2 => pad::R2,
            Button::Start => pad::START,
            Button::Select => pad::SELECT,
            Button::DPadUp => pad::UP,
            Button::DPadDown => pad::DOWN,
            Button::DPadLeft => pad::LEFT,
            Button::DPadRight => pad::RIGHT,
            _ => 0,
        }
    }

    /// Drains pending events; returns the buttons currently held. Presses are also latched into `shared`
    /// so that a tap shorter than a UI frame still reaches the game.
    pub fn poll(&mut self, shared: &Shared) -> u16 {
        let Some(g) = &mut self.gilrs else { return 0 };
        while let Some(ev) = g.next_event() {
            match ev.event {
                EventType::ButtonPressed(b, _) => {
                    let bit = Self::bit(b);
                    self.held |= bit;
                    shared.user_latch.fetch_or(bit, Ordering::Relaxed);
                }
                EventType::ButtonReleased(b, _) => self.held &= !Self::bit(b),
                EventType::AxisChanged(Axis::LeftStickX, v, _) => {
                    self.stick &= !(pad::LEFT | pad::RIGHT);
                    if v < -0.5 {
                        self.stick |= pad::LEFT;
                    } else if v > 0.5 {
                        self.stick |= pad::RIGHT;
                    }
                }
                EventType::AxisChanged(Axis::LeftStickY, v, _) => {
                    self.stick &= !(pad::UP | pad::DOWN);
                    if v > 0.5 {
                        self.stick |= pad::UP;
                    } else if v < -0.5 {
                        self.stick |= pad::DOWN;
                    }
                }
                EventType::Disconnected => {
                    self.held = 0;
                    self.stick = 0;
                }
                _ => {}
            }
        }
        self.held | self.stick
    }
}
