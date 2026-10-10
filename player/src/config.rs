//! Settings stored between runs.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;

/// Pad buttons in the order they are shown in the settings.
pub const BUTTONS: &[(&str, u16)] = &[
    ("Up", vibrt::pad::UP),
    ("Down", vibrt::pad::DOWN),
    ("Left", vibrt::pad::LEFT),
    ("Right", vibrt::pad::RIGHT),
    ("Cross (X)", vibrt::pad::CROSS),
    ("Circle", vibrt::pad::CIRCLE),
    ("Square", vibrt::pad::SQUARE),
    ("Triangle", vibrt::pad::TRIANGLE),
    ("L1", vibrt::pad::L1),
    ("R1", vibrt::pad::R1),
    ("L2", vibrt::pad::L2),
    ("R2", vibrt::pad::R2),
    ("Start", vibrt::pad::START),
    ("Select", vibrt::pad::SELECT),
];

/// Default keyboard layout. Vib-Ribbon only uses Down, L1, R1 and Cross, so those sit under the left hand
/// (plus the arrow keys) and everything else is spread out of the way.
fn default_keys() -> BTreeMap<String, String> {
    [
        ("Up", "ArrowUp"),
        ("Down", "ArrowDown"),
        ("Left", "ArrowLeft"),
        ("Right", "ArrowRight"),
        ("Cross (X)", "Z"),
        ("Circle", "X"),
        ("Square", "A"),
        ("Triangle", "S"),
        ("L1", "Q"),
        ("R1", "W"),
        ("L2", "1"),
        ("R2", "2"),
        ("Start", "Enter"),
        ("Select", "Backspace"),
    ]
    .into_iter()
    .map(|(a, b)| (a.to_string(), b.to_string()))
    .collect()
}

#[derive(Serialize, Deserialize, Clone)]
#[serde(default)]
pub struct Config {
    pub disc_path: Option<PathBuf>,
    pub song_path: Option<PathBuf>,
    pub volume: f32,
    pub smooth: bool,
    /// Show the game while a macro is skipping through its menus.
    pub show_macro: bool,
    pub keys: BTreeMap<String, String>,
}

impl Default for Config {
    fn default() -> Self {
        Config { disc_path: None, song_path: None, volume: 0.8, smooth: true, show_macro: false, keys: default_keys() }
    }
}

fn path() -> Option<PathBuf> {
    directories::ProjectDirs::from("", "", "vibribbon-player").map(|d| d.config_dir().join("config.json"))
}

impl Config {
    pub fn load() -> Config {
        let mut c: Config = path()
            .and_then(|p| std::fs::read_to_string(p).ok())
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default();
        for (k, v) in default_keys() {
            c.keys.entry(k).or_insert(v);
        }
        c
    }

    pub fn save(&self) {
        if let Some(p) = path() {
            if let Some(dir) = p.parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            if let Ok(text) = serde_json::to_string_pretty(self) {
                let _ = std::fs::write(p, text);
            }
        }
    }

    pub fn reset_keys(&mut self) {
        self.keys = default_keys();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_keys_resolve_and_are_unique() {
        let keys = default_keys();
        let mut seen = std::collections::HashSet::new();
        for (button, _) in BUTTONS {
            let name = keys.get(*button).unwrap_or_else(|| panic!("no key for {button}"));
            let key = eframe::egui::Key::from_name(name).unwrap_or_else(|| panic!("'{name}' is not a key name"));
            assert!(seen.insert(key), "{name} is bound twice");
            // names written by the settings screen (Key::name) must read back
            assert_eq!(eframe::egui::Key::from_name(key.name()), Some(key));
        }
    }
}
