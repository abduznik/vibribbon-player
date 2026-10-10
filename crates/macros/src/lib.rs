//! Macros: scripted controller input for the unmodified game.
//!
//! A macro is a list of steps, run one after the other. The engine is driven once per video frame
//! ([`Engine::tick`]) and told about CD commands the game sends ([`Engine::on_cd`]); it answers with the
//! controller state to use and a few machine actions. It knows nothing about the emulator itself.

use serde::Deserialize;
use std::collections::BTreeMap;

pub const PAD_SELECT: u16 = 0x0001;
pub const PAD_START: u16 = 0x0008;
pub const PAD_UP: u16 = 0x0010;
pub const PAD_RIGHT: u16 = 0x0020;
pub const PAD_DOWN: u16 = 0x0040;
pub const PAD_LEFT: u16 = 0x0080;
pub const PAD_L2: u16 = 0x0100;
pub const PAD_R2: u16 = 0x0200;
pub const PAD_L1: u16 = 0x0400;
pub const PAD_R1: u16 = 0x0800;
pub const PAD_TRIANGLE: u16 = 0x1000;
pub const PAD_CIRCLE: u16 = 0x2000;
pub const PAD_CROSS: u16 = 0x4000;
pub const PAD_SQUARE: u16 = 0x8000;

fn button(name: &str) -> Option<u16> {
    Some(match name.to_ascii_lowercase().as_str() {
        "select" => PAD_SELECT,
        "start" => PAD_START,
        "up" => PAD_UP,
        "right" => PAD_RIGHT,
        "down" => PAD_DOWN,
        "left" => PAD_LEFT,
        "l2" => PAD_L2,
        "r2" => PAD_R2,
        "l1" => PAD_L1,
        "r1" => PAD_R1,
        "triangle" => PAD_TRIANGLE,
        "circle" => PAD_CIRCLE,
        "cross" | "x" => PAD_CROSS,
        "square" => PAD_SQUARE,
        _ => return None,
    })
}

fn cd_command(name: &str) -> Option<u8> {
    Some(match name.to_ascii_lowercase().as_str() {
        "setloc" => 0x02,
        "play" => 0x03,
        "readn" => 0x06,
        "mute" => 0x0b,
        "demute" => 0x0c,
        "getlocp" => 0x11,
        "seekp" => 0x16,
        other => {
            let hex = other.strip_prefix("0x")?;
            u8::from_str_radix(hex, 16).ok()?
        }
    })
}

#[derive(Debug, Clone, Deserialize)]
#[serde(untagged)]
pub enum Buttons {
    One(String),
    Many(Vec<String>),
}

/// One step. Exactly one "primary" key (`wait`, `wait_until`, `wait_cd`, `press`, `swap_song`) or only the
/// flag keys (`fast_forward`, `turbo`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Step {
    /// Do nothing for this many frames (counted from the start of the step).
    pub wait: Option<u32>,
    /// Do nothing until this frame number since the start of the run.
    pub wait_until: Option<u32>,
    /// Wait until the game next sends this CD command (e.g. "play"). `timeout` is in frames.
    pub wait_cd: Option<String>,
    /// Press buttons so the game sees them for `hold` frames (default 1: a tap) and carry on. With `every` + `until_cd` it repeats the
    /// press every `every` frames and waits for the CD command; `timeout` (frames) fails the macro.
    pub press: Option<Buttons>,
    pub hold: Option<u32>,
    pub every: Option<u32>,
    pub until_cd: Option<String>,
    pub timeout: Option<u32>,
    /// Swap the song in as the disc (lid open for `open_ms`, default 500).
    pub swap_song: Option<bool>,
    pub open_ms: Option<u32>,
    pub fast_forward: Option<bool>,
    pub turbo: Option<bool>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct MacroDef {
    pub title: String,
    #[serde(default)]
    pub needs_song: bool,
    /// Executable to start directly instead of the normal boot (loader + title screen).
    pub boot_exe: Option<String>,
    pub steps: Vec<Step>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct DiscMatch {
    pub name: String,
    /// Executables named by SYSTEM.CNF that identify the disc.
    pub boot: Vec<String>,
    #[serde(default)]
    pub pal: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Profile {
    pub disc: DiscMatch,
    #[serde(rename = "macro")]
    pub macros: BTreeMap<String, MacroDef>,
}

const BUILTIN_PROFILES: &[&str] = &[include_str!("../profiles/sces_028_73.toml")];

#[derive(Debug)]
pub struct ProfileError(pub String);
impl std::fmt::Display for ProfileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for ProfileError {}

impl Profile {
    pub fn parse(text: &str) -> Result<Profile, ProfileError> {
        let p: Profile = toml::from_str(text).map_err(|e| ProfileError(e.to_string()))?;
        for (name, m) in &p.macros {
            for (i, s) in m.steps.iter().enumerate() {
                s.validate().map_err(|e| ProfileError(format!("macro '{name}' step {}: {e}", i + 1)))?;
            }
        }
        Ok(p)
    }

    /// All profiles shipped with the program.
    pub fn builtin() -> Vec<Profile> {
        BUILTIN_PROFILES.iter().map(|t| Profile::parse(t).expect("built-in profile is valid")).collect()
    }

    /// The built-in profile for a disc whose SYSTEM.CNF boots `boot_name`.
    pub fn for_boot_name(boot_name: &str) -> Option<Profile> {
        Profile::builtin().into_iter().find(|p| p.disc.boot.iter().any(|b| b.eq_ignore_ascii_case(boot_name)))
    }
}

impl Step {
    fn validate(&self) -> Result<(), String> {
        let primaries = [
            self.wait.is_some(),
            self.wait_until.is_some(),
            self.wait_cd.is_some(),
            self.press.is_some(),
            self.swap_song.is_some(),
        ]
        .iter()
        .filter(|b| **b)
        .count();
        let flags = self.fast_forward.is_some() || self.turbo.is_some();
        if primaries > 1 || (primaries == 0 && !flags) {
            return Err("a step needs exactly one of wait, wait_until, wait_cd, press, swap_song (or flags)".into());
        }
        if let Some(b) = &self.press {
            for n in b.names() {
                button(n).ok_or_else(|| format!("unknown button '{n}'"))?;
            }
        }
        for c in self.wait_cd.iter().chain(self.until_cd.iter()) {
            cd_command(c).ok_or_else(|| format!("unknown CD command '{c}'"))?;
        }
        if self.until_cd.is_some() && self.press.is_none() {
            return Err("until_cd goes with press".into());
        }
        Ok(())
    }
}

impl Buttons {
    fn names(&self) -> Vec<&str> {
        match self {
            Buttons::One(s) => vec![s.as_str()],
            Buttons::Many(v) => v.iter().map(String::as_str).collect(),
        }
    }
    fn mask(&self) -> u16 {
        self.names().iter().filter_map(|n| button(n)).fold(0, |a, b| a | b)
    }
}

/// What the machine should do after a tick.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct TickOut {
    /// Controller state to apply while the macro is running (pressed bits).
    pub pad: u16,
    /// Swap the song in now; the value is how long the lid stays open (emulated ms).
    pub swap_song: Option<u32>,
    pub fast_forward: Option<bool>,
    pub turbo: Option<bool>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Status {
    Running,
    Done,
    Failed(String),
}

pub struct Engine {
    steps: Vec<Step>,
    idx: usize,
    status: Status,
    /// Frame at which the current step began (see `Step` semantics).
    t0: u32,
    started: bool,
    press_mask: u16,
    press_until: u32,
    next_press: u32,
    /// CD commands seen since the current step began.
    seen: Vec<u8>,
    last_frame: u32,
}

impl Engine {
    pub fn new(m: &MacroDef) -> Engine {
        Engine {
            steps: m.steps.clone(),
            idx: 0,
            status: if m.steps.is_empty() { Status::Done } else { Status::Running },
            t0: 0,
            started: false,
            press_mask: 0,
            press_until: 0,
            next_press: 0,
            seen: Vec::new(),
            last_frame: 0,
        }
    }

    pub fn status(&self) -> &Status {
        &self.status
    }

    /// True while the macro owns the controller.
    pub fn running(&self) -> bool {
        self.status == Status::Running
    }

    /// The game sent a CD command (call from the emulation thread between ticks).
    pub fn on_cd(&mut self, cmd: u8) {
        if self.running() {
            self.seen.push(cmd);
        }
    }

    fn advance(&mut self, frame: u32) {
        self.idx += 1;
        self.started = false;
        self.t0 = frame;
        self.seen.clear();
        if self.idx >= self.steps.len() {
            self.status = Status::Done;
        }
    }

    /// Call once per video frame, before the game reads the pad. `frame` counts from 1.
    pub fn tick(&mut self, frame: u32) -> TickOut {
        let mut out = TickOut::default();
        if !self.running() {
            return out;
        }
        // a press started in an earlier frame ends (or is still held) before this frame's decisions
        if self.press_mask != 0 && frame >= self.press_until {
            self.press_mask = 0;
        }
        out.pad = self.press_mask;
        if self.idx == 0 && !self.started {
            self.t0 = frame;
        }
        // run as many steps as complete within this tick
        while self.running() {
            let step = self.steps[self.idx].clone();
            let first = !self.started;
            self.started = true;
            let done = self.run_step(&step, frame, first, &mut out);
            if let Status::Failed(_) = self.status {
                break;
            }
            if !done {
                break;
            }
            // completion by a CD event is credited to the frame the event happened in
            let at = if step.wait_cd.is_some() { self.last_frame.min(frame) } else { frame };
            self.advance(at);
        }
        self.last_frame = frame;
        out
    }

    fn run_step(&mut self, s: &Step, frame: u32, first: bool, out: &mut TickOut) -> bool {
        if first {
            self.last_frame = frame;
        }
        if let Some(n) = s.wait {
            return frame >= self.t0 + n;
        }
        if let Some(n) = s.wait_until {
            return frame >= n;
        }
        if let Some(c) = &s.wait_cd {
            let want = cd_command(c).unwrap();
            if self.seen.contains(&want) {
                return true;
            }
            if let Some(t) = s.timeout {
                if frame >= self.t0 + t {
                    self.status = Status::Failed(format!("the game never sent the CD command '{c}'"));
                }
            }
            return false;
        }
        if let Some(b) = &s.press {
            let hold = s.hold.unwrap_or(1);
            if let Some(until) = &s.until_cd {
                let want = cd_command(until).unwrap();
                if self.seen.contains(&want) {
                    return true;
                }
                if let Some(t) = s.timeout {
                    if frame >= self.t0 + t {
                        self.status = Status::Failed(format!("the game never reached the CD command '{until}'"));
                        return false;
                    }
                }
                if first || frame >= self.next_press {
                    self.press_mask = b.mask();
                    self.press_until = frame + hold + 1;
                    self.next_press = frame + s.every.unwrap_or(40);
                }
                return false;
            }
            // fire and carry on: the button is held for `hold` frames while later steps run
            self.press_mask = b.mask();
            self.press_until = frame + hold + 1;
            return true;
        }
        if s.swap_song == Some(true) {
            out.swap_song = Some(s.open_ms.unwrap_or(500));
            return true;
        }
        if s.fast_forward.is_some() || s.turbo.is_some() {
            if s.fast_forward.is_some() {
                out.fast_forward = s.fast_forward;
            }
            if s.turbo.is_some() {
                out.turbo = s.turbo;
            }
            return true;
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn engine(steps: &str) -> Engine {
        let p = Profile::parse(&format!("[disc]\nname='t'\nboot=['X']\n[macro.m]\ntitle='t'\nsteps = [{steps}]\n"))
            .unwrap();
        Engine::new(&p.macros["m"])
    }

    #[test]
    fn builtin_profile_parses() {
        let p = Profile::for_boot_name("SCES_028.73").expect("profile");
        assert!(p.macros.contains_key("song") && p.macros.contains_key("how_to_play"));
        assert!(p.macros["song"].needs_song);
    }

    #[test]
    fn press_starts_on_the_tick_after_the_step() {
        let mut e = engine("{ wait_until = 3 }, { press = 'cross' }, { wait_until = 10 }");
        let pads: Vec<u16> = (1..=8).map(|f| e.tick(f).pad).collect();
        // a tap: the game sees it for exactly one frame (the tick that starts it still reports the old state)
        assert_eq!(pads, vec![0, 0, 0, PAD_CROSS, 0, 0, 0, 0]);

        let mut e = engine("{ wait_until = 3 }, { press = ['l1', 'down'], hold = 3 }, { wait_until = 10 }");
        let pads: Vec<u16> = (1..=9).map(|f| e.tick(f).pad).collect();
        assert_eq!(pads, vec![0, 0, 0, PAD_L1 | PAD_DOWN, PAD_L1 | PAD_DOWN, PAD_L1 | PAD_DOWN, 0, 0, 0]);
    }

    #[test]
    fn wait_cd_then_relative_wait_counts_from_the_event_frame() {
        let mut e = engine("{ wait_cd = 'play' }, { wait = 5 }, { swap_song = true }");
        for f in 1..=10 {
            e.tick(f);
        }
        e.on_cd(0x03); // happens during frame 10
        let mut swapped_at = None;
        for f in 11..=30 {
            if e.tick(f).swap_song.is_some() {
                swapped_at = Some(f);
            }
        }
        assert_eq!(swapped_at, Some(15));
        assert_eq!(e.status(), &Status::Done);
    }

    #[test]
    fn retry_press_until_cd_and_timeout() {
        let mut e = engine("{ press = 'cross', every = 4, until_cd = 'mute', timeout = 20 }");
        let mut presses = 0;
        let mut prev = 0;
        for f in 1..=30 {
            let p = e.tick(f).pad;
            if p != 0 && prev == 0 {
                presses += 1;
            }
            prev = p;
        }
        assert!(presses >= 3);
        assert!(matches!(e.status(), Status::Failed(_)));

        let mut e =
            engine("{ press = 'cross', every = 4, until_cd = 'mute', timeout = 100 }, { fast_forward = false }");
        for f in 1..=10 {
            e.tick(f);
        }
        e.on_cd(0x0b);
        let out = e.tick(11);
        assert_eq!(out.fast_forward, Some(false));
        assert_eq!(e.status(), &Status::Done);
    }

    #[test]
    fn validation_catches_mistakes() {
        assert!(Profile::parse("[disc]\nname='t'\nboot=['X']\n[macro.m]\ntitle='t'\nsteps=[{ press='nope' }]").is_err());
        assert!(Profile::parse(
            "[disc]\nname='t'\nboot=['X']\n[macro.m]\ntitle='t'\nsteps=[{ wait=1, press='cross' }]"
        )
        .is_err());
        assert!(
            Profile::parse("[disc]\nname='t'\nboot=['X']\n[macro.m]\ntitle='t'\nsteps=[{ wait_cd='bogus' }]").is_err()
        );
    }
}
