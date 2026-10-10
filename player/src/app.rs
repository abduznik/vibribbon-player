//! The window: launcher (pick disc and song, press play), game view, settings.

use crate::config::{Config, BUTTONS};
use crate::session::{Event, Params, Session, FLUSH_AUDIO, MUTE};
use eframe::egui::{self, Color32, Key, RichText, TextureHandle, TextureOptions};
use macros::Profile;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::{channel, Receiver};
use std::sync::Arc;
use std::time::{Duration, Instant};

static VOLUME: AtomicU32 = AtomicU32::new(0x3f4c_cccd); // 0.8

const DISC_EXTS: &[&str] = &["cue", "chd"];
const SONG_EXTS: &[&str] =
    &["mp3", "m4a", "aac", "mp4", "flac", "ogg", "oga", "wav", "wave", "mka", "caf", "aif", "aiff"];

// ------------------------------------------------------------------ background results
struct DiscInfo {
    path: PathBuf,
    name: String,
    boot: String,
    profile: Option<Profile>,
}

enum DiscState {
    None,
    Probing(PathBuf),
    Ready(DiscInfo),
    Error(PathBuf, String),
}

struct SongInfo {
    title: String,
    seconds: f32,
    pcm: Arc<Vec<i16>>,
}

enum SongState {
    None,
    Loading(PathBuf),
    Ready(SongInfo),
    Error(PathBuf, String),
}

fn probe_disc(path: &Path) -> Result<DiscInfo, String> {
    let img = media::open_disc(path).map_err(|e| e.to_string())?;
    let label = img.label.clone();
    let disc = img.into_disc().ok_or("the disc image could not be opened")?;
    let boot = disc.boot_name().ok_or("this does not look like a PlayStation disc (no SYSTEM.CNF)")?;
    let profile = Profile::for_boot_name(&boot);
    let name = profile.as_ref().map(|p| p.disc.name.clone()).unwrap_or(label);
    Ok(DiscInfo { path: path.to_path_buf(), name, boot, profile })
}

fn load_song(path: &Path) -> Result<SongInfo, String> {
    let s = media::decode_song(path).map_err(|e| e.to_string())?;
    Ok(SongInfo { seconds: s.seconds(), title: s.title, pcm: Arc::new(s.pcm) })
}

fn ext_of(p: &Path) -> String {
    p.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).unwrap_or_default()
}

// ------------------------------------------------------------------ app state
struct Playing {
    session: Session,
    macro_key: String,
    macro_title: String,
    menu: bool,
    failure: Option<String>,
}

enum Screen {
    Launcher,
    Playing(Box<Playing>),
}

struct Toast {
    text: String,
    error: bool,
    until: Instant,
}

pub struct App {
    cfg: Config,
    disc: DiscState,
    song: SongState,
    rx_disc: Option<Receiver<Result<DiscInfo, String>>>,
    rx_song: Option<Receiver<Result<SongInfo, String>>>,
    screen: Screen,
    _audio: Option<audioout::AudioOut>,
    audio_error: Option<String>,
    toast: Option<Toast>,
    show_settings: bool,
    rebinding: Option<String>,
    key_map: HashMap<Key, u16>,
    held: u16,
    texture: Option<TextureHandle>,
    logo: Option<TextureHandle>,
    last_seq: u64,
    fullscreen: bool,
    pending_restart: bool,
    autostart: Option<&'static str>,
    #[cfg(feature = "gamepad")]
    pads: crate::gamepad::Pads,
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>) -> App {
        let cfg = Config::load();
        VOLUME.store(cfg.volume.to_bits(), Ordering::Relaxed);
        crate::branding::apply_theme(&cc.egui_ctx);
        let logo = crate::branding::decode_png(crate::branding::LOGO_PNG).map(|i| {
            cc.egui_ctx.load_texture(
                "logo",
                egui::ColorImage::from_rgba_unmultiplied([i.width, i.height], &i.rgba),
                TextureOptions::LINEAR,
            )
        });

        let (audio, audio_error) = match audioout::AudioOut::open(|buf| {
            if FLUSH_AUDIO.swap(false, Ordering::Relaxed) {
                vibrt::audio::clear();
            }
            let frames = vibrt::audio::pull(buf);
            if MUTE.load(Ordering::Relaxed) {
                buf.fill(0);
                return;
            }
            let vol = f32::from_bits(VOLUME.load(Ordering::Relaxed));
            if vol < 0.999 {
                for s in &mut buf[..frames * 2] {
                    *s = (*s as f32 * vol) as i16;
                }
            }
            buf[frames * 2..].fill(0);
        }) {
            Ok(a) => (Some(a), None),
            Err(e) => (None, Some(e)),
        };

        let mut app = App {
            cfg,
            disc: DiscState::None,
            song: SongState::None,
            rx_disc: None,
            rx_song: None,
            screen: Screen::Launcher,
            _audio: audio,
            audio_error,
            toast: None,
            show_settings: false,
            rebinding: None,
            key_map: HashMap::new(),
            held: 0,
            texture: None,
            logo,
            last_seq: 0,
            fullscreen: false,
            pending_restart: false,
            autostart: None,
            #[cfg(feature = "gamepad")]
            pads: crate::gamepad::Pads::new(),
        };
        app.rebuild_key_map();
        if let Some(p) = app.cfg.disc_path.clone() {
            app.set_disc(&cc.egui_ctx, p);
        }
        if let Some(p) = app.cfg.song_path.clone() {
            app.set_song(&cc.egui_ctx, p);
        }
        app
    }

    /// Command line: files (disc image, song), `--disc F`, `--song F`, `--play` (start right away),
    /// `--tutorial` (start the how-to-play macro right away).
    pub fn with_args(mut self, ctx: &egui::Context, args: &[String]) -> App {
        let mut it = args.iter();
        while let Some(a) = it.next() {
            match a.as_str() {
                "--play" => self.autostart = Some("song"),
                "--tutorial" => self.autostart = Some("how_to_play"),
                "--disc" | "--song" => {
                    if let Some(f) = it.next() {
                        self.accept_file(ctx, PathBuf::from(f));
                    }
                }
                f if !f.starts_with("--") => self.accept_file(ctx, PathBuf::from(f)),
                _ => {}
            }
        }
        if std::env::var_os("VIBRIBBON_MUTE").is_some() {
            VOLUME.store(0f32.to_bits(), Ordering::Relaxed);
        }
        self
    }

    fn rebuild_key_map(&mut self) {
        self.key_map.clear();
        for (name, bit) in BUTTONS {
            if let Some(key) = self.cfg.keys.get(*name).and_then(|k| Key::from_name(k)) {
                *self.key_map.entry(key).or_insert(0) |= *bit;
            }
        }
    }

    fn say(&mut self, text: impl Into<String>, error: bool) {
        self.toast = Some(Toast { text: text.into(), error, until: Instant::now() + Duration::from_secs(6) });
    }

    // ---------------------------------------------------------- inputs: disc and song
    fn set_disc(&mut self, ctx: &egui::Context, path: PathBuf) {
        let (tx, rx) = channel();
        let ctx2 = ctx.clone();
        let p2 = path.clone();
        std::thread::spawn(move || {
            let _ = tx.send(probe_disc(&p2));
            ctx2.request_repaint();
        });
        self.rx_disc = Some(rx);
        self.disc = DiscState::Probing(path.clone());
        self.cfg.disc_path = Some(path);
        self.cfg.save();
    }

    fn set_song(&mut self, ctx: &egui::Context, path: PathBuf) {
        let (tx, rx) = channel();
        let ctx2 = ctx.clone();
        let p2 = path.clone();
        std::thread::spawn(move || {
            let _ = tx.send(load_song(&p2));
            ctx2.request_repaint();
        });
        self.rx_song = Some(rx);
        self.song = SongState::Loading(path.clone());
        self.cfg.song_path = Some(path);
        self.cfg.save();
    }

    fn accept_file(&mut self, ctx: &egui::Context, path: PathBuf) {
        let ext = ext_of(&path);
        if DISC_EXTS.contains(&ext.as_str()) {
            self.set_disc(ctx, path);
        } else if SONG_EXTS.contains(&ext.as_str()) || !ext.is_empty() {
            let restart = matches!(&self.screen, Screen::Playing(p) if p.macro_key == "song");
            self.set_song(ctx, path);
            if restart {
                self.stop_game();
                self.pending_restart = true;
            }
        }
    }

    fn poll_background(&mut self) {
        if let Some(rx) = &self.rx_disc {
            if let Ok(r) = rx.try_recv() {
                self.rx_disc = None;
                self.disc = match r {
                    Ok(info) => DiscState::Ready(info),
                    Err(e) => {
                        let p = self.cfg.disc_path.clone().unwrap_or_default();
                        DiscState::Error(p, e)
                    }
                };
            }
        }
        if let Some(rx) = &self.rx_song {
            if let Ok(r) = rx.try_recv() {
                self.rx_song = None;
                self.song = match r {
                    Ok(info) => SongState::Ready(info),
                    Err(e) => {
                        let p = self.cfg.song_path.clone().unwrap_or_default();
                        SongState::Error(p, e)
                    }
                };
            }
        }
    }

    // ---------------------------------------------------------- running the game
    fn can_start(&self, key: &str) -> Result<(), String> {
        let DiscState::Ready(info) = &self.disc else { return Err("Choose your Vib-Ribbon disc image first.".into()) };
        let Some(profile) = &info.profile else {
            return Err(format!(
                "This disc ({}) is not supported yet, so the game cannot be set up for you.",
                info.boot
            ));
        };
        let Some(m) = profile.macros.get(key) else { return Err("This disc has no such setup.".into()) };
        if m.needs_song {
            match &self.song {
                SongState::Ready(_) => {}
                SongState::Loading(_) => return Err("The song is still being read.".into()),
                _ => return Err("Choose a song first.".into()),
            }
        }
        Ok(())
    }

    fn start_game(&mut self, key: &str) {
        if let Err(e) = self.can_start(key) {
            self.say(e, true);
            return;
        }
        let DiscState::Ready(info) = &self.disc else { return };
        let profile = info.profile.clone().unwrap();
        let mdef = profile.macros[key].clone();
        let game = match media::open_disc(&info.path).map(|d| d.into_disc()) {
            Ok(Some(d)) => d,
            Ok(None) => return self.say("The disc image could not be opened.", true),
            Err(e) => return self.say(e.to_string(), true),
        };
        let song = if mdef.needs_song {
            match &self.song {
                SongState::Ready(s) => Some(vibrt::Disc::from_pcm(&s.pcm, &s.title)),
                _ => None,
            }
        } else {
            None
        };
        FLUSH_AUDIO.store(true, Ordering::Relaxed);
        MUTE.store(false, Ordering::Relaxed);
        let title = mdef.title.clone();
        let session = Session::start(Params {
            game,
            song,
            macro_def: Some(mdef),
            pal: profile.disc.pal,
            headless: false,
            on_frame: None,
        });
        self.texture = None;
        self.last_seq = 0;
        self.held = 0;
        self.screen = Screen::Playing(Box::new(Playing {
            session,
            macro_key: key.to_string(),
            macro_title: title,
            menu: false,
            failure: None,
        }));
    }

    fn stop_game(&mut self) {
        if let Screen::Playing(p) = &mut self.screen {
            p.session.stop();
        }
        self.screen = Screen::Launcher;
        MUTE.store(false, Ordering::Relaxed);
        FLUSH_AUDIO.store(true, Ordering::Relaxed);
        self.texture = None;
    }

    // ---------------------------------------------------------- input while playing
    fn gather_input(&mut self, ctx: &egui::Context) {
        let Screen::Playing(p) = &mut self.screen else { return };
        let (events, focused) = ctx.input(|i| (i.events.clone(), i.focused));
        let mut esc = false;
        for ev in events {
            if let egui::Event::Key { key, pressed, repeat, .. } = ev {
                if key == Key::Escape && pressed && !repeat {
                    esc = true;
                }
                if key == Key::F11 && pressed && !repeat {
                    self.fullscreen = !self.fullscreen;
                    ctx.send_viewport_cmd(egui::ViewportCommand::Fullscreen(self.fullscreen));
                }
                if repeat || p.menu {
                    continue;
                }
                if let Some(&bit) = self.key_map.get(&key) {
                    if pressed {
                        self.held |= bit;
                        p.session.shared.user_latch.fetch_or(bit, Ordering::Relaxed);
                    } else {
                        self.held &= !bit;
                    }
                }
            }
        }
        if !focused || p.menu {
            self.held = 0;
        }
        if esc {
            p.menu = !p.menu;
            p.session.shared.paused.store(p.menu, Ordering::Relaxed);
            if p.menu {
                self.held = 0;
            }
        }
        #[allow(unused_mut)]
        let mut pad = self.held;
        #[cfg(feature = "gamepad")]
        {
            pad |= self.pads.poll(&p.session.shared);
        }
        p.session.shared.user_pad.store(pad, Ordering::Relaxed);
    }
}

// ------------------------------------------------------------------ drawing
fn status_line(ui: &mut egui::Ui, ok: Option<bool>, text: impl Into<String>) {
    let (icon, color) = match ok {
        Some(true) => ("\u{2714}", Color32::from_rgb(110, 200, 120)),
        Some(false) => ("\u{26A0}", Color32::from_rgb(230, 170, 70)),
        None => ("\u{2026}", Color32::GRAY),
    };
    ui.horizontal_wrapped(|ui| {
        ui.label(RichText::new(icon).color(color));
        ui.label(RichText::new(text.into()).color(color));
    });
}

fn fmt_time(secs: f32) -> String {
    let s = secs.round() as u32;
    format!("{}:{:02}", s / 60, s % 60)
}

impl App {
    fn ui_launcher(&mut self, ctx: &egui::Context) {
        let mut pick_disc = false;
        let mut pick_song = false;
        let mut start: Option<&'static str> = None;
        let mut open_settings = false;

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.set_max_width(640.0);
                ui.add_space(10.0);
                if let Some(logo) = &self.logo {
                    ui.add(egui::Image::new((logo.id(), egui::vec2(128.0, 128.0))));
                }
                ui.label(RichText::new("Vib-Ribbon Player").size(28.0).strong());
                ui.label(RichText::new(format!("Play Vib-Ribbon with any song you like   \u{00B7}   v{}", env!("CARGO_PKG_VERSION"))).color(Color32::GRAY));
                ui.add_space(12.0);
            });
            ui.vertical_centered(|ui| {
                ui.set_max_width(640.0);

                // ---- game disc
                egui::Frame::group(ui.style()).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.label(RichText::new("1.  Your Vib-Ribbon disc").strong());
                    ui.add_space(4.0);
                    ui.horizontal(|ui| {
                        if ui.button("Choose disc image\u{2026}").clicked() {
                            pick_disc = true;
                        }
                        ui.label(RichText::new(".cue (with its .bin files) or .chd").weak());
                    });
                    match &self.disc {
                        DiscState::None => status_line(ui, None, "Drop the disc image here, or press the button."),
                        DiscState::Probing(p) => status_line(ui, None, format!("Reading {}\u{2026}", p.file_name().unwrap_or_default().to_string_lossy())),
                        DiscState::Error(p, e) => status_line(ui, Some(false), format!("{}: {e}", p.file_name().unwrap_or_default().to_string_lossy())),
                        DiscState::Ready(info) if info.profile.is_some() => status_line(ui, Some(true), info.name.clone()),
                        DiscState::Ready(info) => status_line(ui, Some(false), format!("Not a supported Vib-Ribbon disc ({}).", info.boot)),
                    }
                });
                ui.add_space(10.0);

                // ---- song
                egui::Frame::group(ui.style()).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.label(RichText::new("2.  Your song").strong());
                    ui.add_space(4.0);
                    ui.horizontal(|ui| {
                        if ui.button("Choose song\u{2026}").clicked() {
                            pick_song = true;
                        }
                        ui.label(RichText::new("mp3, m4a, flac, ogg, wav\u{2026}").weak());
                    });
                    match &self.song {
                        SongState::None => status_line(ui, None, "Drop an audio file here, or press the button."),
                        SongState::Loading(p) => status_line(ui, None, format!("Reading {}\u{2026}", p.file_name().unwrap_or_default().to_string_lossy())),
                        SongState::Error(p, e) => status_line(ui, Some(false), format!("{}: {e}", p.file_name().unwrap_or_default().to_string_lossy())),
                        SongState::Ready(s) => status_line(ui, Some(true), format!("{}  ({})", s.title, fmt_time(s.seconds))),
                    }
                });
                ui.add_space(16.0);

                // ---- play
                let can_song = self.can_start("song").is_ok();
                let can_tut = self.can_start("how_to_play").is_ok();
                let big = egui::vec2(300.0, 48.0);
                if ui.add_enabled(can_song, egui::Button::new(RichText::new("\u{25B6}  Play with my song").size(20.0)).min_size(big)).clicked() {
                    start = Some("song");
                }
                if let Err(why) = self.can_start("song") {
                    ui.label(RichText::new(why).weak().small());
                }
                ui.add_space(8.0);
                if ui.add_enabled(can_tut, egui::Button::new("How to play  (the game\u{2019}s own tutorial)").min_size(egui::vec2(300.0, 32.0))).clicked() {
                    start = Some("how_to_play");
                }
                ui.add_space(14.0);
                if ui.button("Settings & controls").clicked() {
                    open_settings = true;
                }
                if let Some(e) = &self.audio_error {
                    ui.add_space(8.0);
                    status_line(ui, Some(false), format!("No sound: {e}"));
                }
                ui.add_space(18.0);
                ui.label(
                    RichText::new("This program contains no game data. You need your own copy of Vib-Ribbon; the game runs unmodified.")
                        .weak()
                        .small(),
                );
            });
        });

        if pick_disc {
            if let Some(p) = rfd::FileDialog::new()
                .add_filter("Disc image", DISC_EXTS)
                .set_title("Choose your Vib-Ribbon disc image")
                .pick_file()
            {
                self.set_disc(ctx, p);
            }
        }
        if pick_song {
            if let Some(p) =
                rfd::FileDialog::new().add_filter("Audio", SONG_EXTS).set_title("Choose a song").pick_file()
            {
                self.set_song(ctx, p);
            }
        }
        if open_settings {
            self.show_settings = true;
        }
        if let Some(k) = start {
            self.start_game(k);
        }
    }

    fn ui_settings(&mut self, ctx: &egui::Context) {
        let mut open = self.show_settings;
        let mut changed_keys = false;
        let mut save = false;
        egui::Window::new("Settings & controls").open(&mut open).collapsible(false).resizable(false).show(ctx, |ui| {
            ui.label(RichText::new("Sound").strong());
            if ui.add(egui::Slider::new(&mut self.cfg.volume, 0.0..=1.0).text("Volume")).changed() {
                VOLUME.store(self.cfg.volume.to_bits(), Ordering::Relaxed);
                save = true;
            }
            ui.add_space(6.0);
            ui.label(RichText::new("Picture").strong());
            if ui.checkbox(&mut self.cfg.smooth, "Smooth scaling").changed() {
                self.texture = None;
                save = true;
            }
            if ui.checkbox(&mut self.cfg.show_macro, "Show the game while it is being set up for you").changed() {
                save = true;
            }
            ui.add_space(6.0);
            ui.label(RichText::new("Keyboard").strong());
            ui.label(
                RichText::new("Vib-Ribbon uses Down, L1, R1 and Cross. Click a key, then press the new one.")
                    .weak()
                    .small(),
            );
            egui::Grid::new("keys").num_columns(4).spacing([18.0, 4.0]).show(ui, |ui| {
                for (i, (name, _)) in BUTTONS.iter().enumerate() {
                    ui.label(*name);
                    let current = self.cfg.keys.get(*name).cloned().unwrap_or_default();
                    let waiting = self.rebinding.as_deref() == Some(*name);
                    let label = if waiting { "press a key\u{2026}".to_string() } else { current };
                    if ui.add(egui::Button::new(label).min_size(egui::vec2(96.0, 0.0)).selected(waiting)).clicked() {
                        self.rebinding = Some(name.to_string());
                    }
                    if i % 2 == 1 {
                        ui.end_row();
                    }
                }
            });
            if ui.button("Reset keys").clicked() {
                self.cfg.reset_keys();
                changed_keys = true;
            }
            ui.add_space(4.0);
            ui.label(RichText::new("Esc: menu / pause     F11: full screen").weak().small());
        });
        // capture the key being rebound
        if let Some(name) = self.rebinding.clone() {
            let pressed = ctx.input(|i| {
                i.events.iter().find_map(|e| match e {
                    egui::Event::Key { key, pressed: true, repeat: false, .. } => Some(*key),
                    _ => None,
                })
            });
            if let Some(key) = pressed {
                if key != Key::Escape {
                    // a key can only drive one button
                    let k = key.name().to_string();
                    self.cfg.keys.retain(|_, v| *v != k);
                    self.cfg.keys.insert(name, k);
                    changed_keys = true;
                }
                self.rebinding = None;
            }
        }
        if changed_keys {
            self.rebuild_key_map();
            save = true;
        }
        if save {
            self.cfg.save();
        }
        self.show_settings = open;
        if !open {
            self.rebinding = None;
        }
    }

    fn ui_game(&mut self, ctx: &egui::Context) {
        let show_macro = self.cfg.show_macro;
        let opts = if self.cfg.smooth { TextureOptions::LINEAR } else { TextureOptions::NEAREST };
        let Screen::Playing(p) = &mut self.screen else { return };

        // ---- events from the emulation
        let mut leave: Option<String> = None;
        for ev in p.session.shared.take_events() {
            match ev {
                Event::MacroDone { .. } => {}
                Event::MacroFailed { reason, .. } => p.failure = Some(reason),
                Event::Exited { .. } => leave = Some("The game ended.".into()),
            }
        }

        // ---- new picture
        {
            let f = p.session.shared.frame.lock().unwrap();
            if f.seq != self.last_seq {
                let mut bytes = Vec::with_capacity(f.w * f.h * 4);
                for y in 0..f.h {
                    bytes.extend_from_slice(bytemuck::cast_slice(&f.rgba[y * 1024..y * 1024 + f.w]));
                }
                let img = egui::ColorImage::from_rgba_unmultiplied([f.w, f.h], &bytes);
                match &mut self.texture {
                    Some(t) => t.set(img, opts),
                    None => self.texture = Some(ctx.load_texture("game", img, opts)),
                }
                self.last_seq = f.seq;
            }
        }
        let macro_running = p.session.shared.macro_running.load(Ordering::Relaxed);
        let hide = macro_running && !show_macro;

        enum Act {
            Resume,
            Restart,
            Back,
            Fullscreen,
        }
        let mut act: Option<Act> = None;
        let tex = self.texture.clone();
        egui::CentralPanel::default().frame(egui::Frame::NONE.fill(Color32::BLACK)).show(ctx, |ui| {
            let avail = ui.available_rect_before_wrap();
            // the PlayStation picture is shown at 4:3
            let (mut w, mut h) = (avail.width(), avail.height());
            if w / h > 4.0 / 3.0 {
                w = h * 4.0 / 3.0;
            } else {
                h = w * 3.0 / 4.0;
            }
            let rect = egui::Rect::from_center_size(avail.center(), egui::vec2(w, h));
            if let (Some(t), false) = (&tex, hide) {
                ui.painter().image(
                    t.id(),
                    rect,
                    egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
                    Color32::WHITE,
                );
            }
            if ui.interact(rect, egui::Id::new("game"), egui::Sense::click()).double_clicked() {
                act = Some(Act::Fullscreen);
            }
            if hide {
                ui.painter().text(
                    avail.center() - egui::vec2(0.0, 18.0),
                    egui::Align2::CENTER_CENTER,
                    format!("Setting up \u{201C}{}\u{201D}\u{2026}", p.macro_title),
                    egui::FontId::proportional(22.0),
                    Color32::WHITE,
                );
                ui.painter().text(
                    avail.center() + egui::vec2(0.0, 14.0),
                    egui::Align2::CENTER_CENTER,
                    "the game is being taken to the right menu for you",
                    egui::FontId::proportional(14.0),
                    Color32::GRAY,
                );
                let t = ui.input(|i| i.time) as f32;
                let c = avail.center() + egui::vec2(0.0, 52.0);
                for k in 0..8 {
                    let a = t * 6.0 + k as f32 * std::f32::consts::TAU / 8.0;
                    let alpha = 255 - (k * 28) as u8;
                    ui.painter().circle_filled(
                        c + egui::vec2(a.cos(), a.sin()) * 14.0,
                        3.0,
                        Color32::from_white_alpha(alpha),
                    );
                }
            }
            if let Some(why) = &p.failure {
                egui::Area::new(egui::Id::new("fail")).anchor(egui::Align2::CENTER_TOP, [0.0, 12.0]).show(ctx, |ui| {
                    egui::Frame::popup(ui.style()).show(ui, |ui| {
                        ui.label(
                            RichText::new(format!("Could not set the game up automatically: {why}"))
                                .color(Color32::from_rgb(230, 170, 70)),
                        );
                        ui.label(
                            "The game is running; you can carry on with the controller yourself (Esc for the menu).",
                        );
                    });
                });
            }
            if p.menu {
                egui::Area::new(egui::Id::new("menu")).anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0]).show(
                    ctx,
                    |ui| {
                        egui::Frame::popup(ui.style()).inner_margin(18.0).show(ui, |ui| {
                            ui.set_min_width(220.0);
                            ui.vertical_centered(|ui| {
                                ui.label(RichText::new("Paused").size(22.0).strong());
                                ui.add_space(8.0);
                                if ui.add(egui::Button::new("Resume").min_size(egui::vec2(200.0, 30.0))).clicked() {
                                    act = Some(Act::Resume);
                                }
                                if ui.add(egui::Button::new("Restart").min_size(egui::vec2(200.0, 30.0))).clicked() {
                                    act = Some(Act::Restart);
                                }
                                if ui
                                    .add(egui::Button::new("Full screen (F11)").min_size(egui::vec2(200.0, 30.0)))
                                    .clicked()
                                {
                                    act = Some(Act::Fullscreen);
                                }
                                if ui
                                    .add(
                                        egui::Button::new("Back to the start screen").min_size(egui::vec2(200.0, 30.0)),
                                    )
                                    .clicked()
                                {
                                    act = Some(Act::Back);
                                }
                            });
                        });
                    },
                );
            }
        });

        let key = p.macro_key.clone();
        match act {
            Some(Act::Resume) => {
                p.menu = false;
                p.session.shared.paused.store(false, Ordering::Relaxed);
            }
            Some(Act::Fullscreen) => {
                self.fullscreen = !self.fullscreen;
                ctx.send_viewport_cmd(egui::ViewportCommand::Fullscreen(self.fullscreen));
            }
            Some(Act::Back) => leave = Some(String::new()),
            Some(Act::Restart) => {
                self.stop_game();
                self.start_game(&key);
                return;
            }
            None => {}
        }
        if let Some(msg) = leave {
            self.stop_game();
            if !msg.is_empty() {
                self.say(msg, false);
            }
        }
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_background();
        for f in ctx.input(|i| i.raw.dropped_files.clone()) {
            if let Some(p) = f.path {
                self.accept_file(ctx, p);
            }
        }
        if self.pending_restart && matches!(self.song, SongState::Ready(_)) {
            self.pending_restart = false;
            self.start_game("song");
        }
        if let Some(k) = self.autostart {
            if matches!(self.screen, Screen::Launcher) {
                match self.can_start(k) {
                    Ok(()) => {
                        self.autostart = None;
                        self.start_game(k);
                    }
                    Err(_)
                        if matches!(self.disc, DiscState::Error(..)) || matches!(self.song, SongState::Error(..)) =>
                    {
                        self.autostart = None;
                    }
                    Err(_) => {}
                }
            }
        }
        self.gather_input(ctx);

        match self.screen {
            Screen::Launcher => self.ui_launcher(ctx),
            Screen::Playing(_) => {
                self.ui_game(ctx);
                ctx.request_repaint();
            }
        }
        if self.show_settings {
            self.ui_settings(ctx);
        }
        if let Some(t) = &self.toast {
            if Instant::now() > t.until {
                self.toast = None;
            } else {
                let (text, error) = (t.text.clone(), t.error);
                egui::Area::new(egui::Id::new("toast")).anchor(egui::Align2::CENTER_BOTTOM, [0.0, -16.0]).show(
                    ctx,
                    |ui| {
                        egui::Frame::popup(ui.style()).show(ui, |ui| {
                            ui.label(RichText::new(text).color(if error {
                                Color32::from_rgb(235, 130, 110)
                            } else {
                                Color32::WHITE
                            }));
                        });
                    },
                );
                ctx.request_repaint_after(Duration::from_millis(250));
            }
        }
    }

    fn on_exit(&mut self, _gl: Option<&eframe::glow::Context>) {
        self.stop_game();
        self.cfg.save();
    }
}
