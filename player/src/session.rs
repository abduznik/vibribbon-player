//! A running game: the emulation thread, the macro engine steering it, and what the UI can see of it.

use macros::{Engine, MacroDef, Status};
use std::sync::atomic::{AtomicBool, AtomicU16, AtomicU32, Ordering};

/// Audio is muted while a macro fast-forwards (read by the audio thread).
pub static MUTE: AtomicBool = AtomicBool::new(false);
/// Set to make the audio thread throw away everything queued so far (it is the ring's only consumer, so it
/// does the flush): used when fast-forward ends and when a game starts.
pub static FLUSH_AUDIO: AtomicBool = AtomicBool::new(false);
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use vibrt::{Control, Disc, Emulator, Host};

/// The most recent picture, RGBA (R in the lowest byte of each `u32`), `w`x`h` valid in a 1024-wide buffer.
pub struct Frame {
    pub w: usize,
    pub h: usize,
    pub rgba: Vec<u32>,
    pub seq: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event {
    MacroDone { frame: u32 },
    MacroFailed { frame: u32, reason: String },
    Exited { frames: u32 },
}

/// State shared between the emulation thread and the UI.
pub struct Shared {
    /// Controller buttons the user is holding (pressed bits); ignored while a macro owns the pad.
    pub user_pad: AtomicU16,
    /// Buttons pressed since the game last looked, so that a tap shorter than a UI frame is not lost.
    pub user_latch: AtomicU16,
    /// The emulation waits (and the picture freezes) while this is set.
    pub paused: AtomicBool,
    /// Muted and unthrottled (a macro is skipping through menus).
    pub fast_forward: AtomicBool,
    pub frame_no: AtomicU32,
    pub frame: Mutex<Frame>,
    pub events: Mutex<Vec<Event>>,
    pub macro_running: AtomicBool,
    pub finished: AtomicBool,
}

impl Shared {
    fn new() -> Shared {
        Shared {
            user_pad: AtomicU16::new(0),
            user_latch: AtomicU16::new(0),
            paused: AtomicBool::new(false),
            fast_forward: AtomicBool::new(false),
            frame_no: AtomicU32::new(0),
            frame: Mutex::new(Frame { w: 512, h: 256, rgba: vec![0xff00_0000; 1024 * 512], seq: 0 }),
            events: Mutex::new(Vec::new()),
            macro_running: AtomicBool::new(false),
            finished: AtomicBool::new(false),
        }
    }
    pub fn take_events(&self) -> Vec<Event> {
        std::mem::take(&mut *self.events.lock().unwrap())
    }
}

pub struct Params {
    pub game: Disc,
    /// The song as a one-track audio CD, if any.
    pub song: Option<Disc>,
    pub macro_def: Option<MacroDef>,
    pub pal: bool,
    /// Run flat out (no audio pacing) and render every frame: for tests and headless runs.
    pub headless: bool,
    /// Called on the emulation thread for every rendered frame (frame number, picture).
    pub on_frame: Option<Box<dyn FnMut(u32, &Frame) + Send>>,
}

pub struct Session {
    pub shared: Arc<Shared>,
    thread: Option<JoinHandle<()>>,
}

struct SessionHost {
    shared: Arc<Shared>,
    engine: Option<Engine>,
    headless: bool,
    ff: bool,
    scratch: Vec<u32>,
    on_frame: Option<Box<dyn FnMut(u32, &Frame) + Send>>,
}

impl SessionHost {
    fn publish(&mut self, ctl: &mut Control<'_>, n: u32) {
        let render = self.headless || !self.ff || n % 8 == 0;
        if !render {
            return;
        }
        let (w, h, _on) = ctl.render_frame(&mut self.scratch);
        let mut f = self.shared.frame.lock().unwrap();
        f.w = w;
        f.h = h;
        for y in 0..h {
            f.rgba[y * 1024..y * 1024 + w].copy_from_slice(&self.scratch[y * 1024..y * 1024 + w]);
        }
        f.seq += 1;
        if let Some(cb) = &mut self.on_frame {
            cb(n, &f);
        }
    }
}

impl Host for SessionHost {
    fn frame(&mut self, ctl: &mut Control<'_>, n: u32) {
        self.shared.frame_no.store(n, Ordering::Relaxed);
        while self.shared.paused.load(Ordering::Relaxed) && !vibrt::stop_requested() {
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        let mut pad = self.shared.user_pad.load(Ordering::Relaxed) | self.shared.user_latch.swap(0, Ordering::Relaxed);
        if let Some(engine) = &mut self.engine {
            let was_running = engine.running();
            let out = engine.tick(n);
            if let Some(ms) = out.swap_song {
                ctl.swap_disc(0, ms);
            }
            if let Some(ff) = out.fast_forward {
                self.ff = ff;
                ctl.set_fast_forward(ff);
                if !ff {
                    FLUSH_AUDIO.store(true, Ordering::Relaxed);
                }
                self.shared.fast_forward.store(ff, Ordering::Relaxed);
                MUTE.store(ff, Ordering::Relaxed);
            }
            if let Some(t) = out.turbo {
                ctl.set_cd_turbo(t);
            }
            if engine.running() {
                pad = out.pad;
            }
            if was_running && !engine.running() {
                let ev = match engine.status() {
                    Status::Failed(r) => Event::MacroFailed { frame: n, reason: r.clone() },
                    _ => Event::MacroDone { frame: n },
                };
                if matches!(ev, Event::MacroFailed { .. }) && self.ff {
                    // never leave the user muted and unthrottled
                    self.ff = false;
                    ctl.set_fast_forward(false);
                    ctl.set_cd_turbo(false);
                    self.shared.fast_forward.store(false, Ordering::Relaxed);
                    MUTE.store(false, Ordering::Relaxed);
                    FLUSH_AUDIO.store(true, Ordering::Relaxed);
                }
                self.shared.macro_running.store(false, Ordering::Relaxed);
                self.shared.events.lock().unwrap().push(ev);
            }
        }
        ctl.set_pad(pad);
        self.publish(ctl, n);
    }

    fn cd_command(&mut self, _ctl: &mut Control<'_>, cmd: u8) {
        if let Some(e) = &mut self.engine {
            e.on_cd(cmd);
        }
    }
}

impl Session {
    pub fn start(p: Params) -> Session {
        let shared = Arc::new(Shared::new());
        let engine = p.macro_def.as_ref().map(Engine::new);
        shared.macro_running.store(engine.as_ref().map_or(false, Engine::running), Ordering::Relaxed);
        let boot = p.macro_def.as_ref().and_then(|m| m.boot_exe.clone());
        let th_shared = shared.clone();
        let thread = std::thread::Builder::new()
            .name("emulation".into())
            .stack_size(16 << 20)
            .spawn(move || {
                vibrt::set_pacing(!p.headless);
                FLUSH_AUDIO.store(true, Ordering::Relaxed);
                let mut emu = Emulator::new(p.game, p.song.into_iter().collect());
                let mut host = SessionHost {
                    shared: th_shared.clone(),
                    engine,
                    headless: p.headless,
                    ff: false,
                    scratch: vec![0; 1024 * 512],
                    on_frame: p.on_frame,
                };
                let frames = emu.run(p.pal, boot.as_deref(), &mut host);
                th_shared.events.lock().unwrap().push(Event::Exited { frames });
                th_shared.finished.store(true, Ordering::SeqCst);
            })
            .expect("spawn emulation thread");
        Session { shared, thread: Some(thread) }
    }

    pub fn is_finished(&self) -> bool {
        self.shared.finished.load(Ordering::SeqCst)
    }

    /// Stops the game and waits for the emulation thread.
    pub fn stop(&mut self) {
        if let Some(t) = self.thread.take() {
            vibrt::stop();
            let _ = t.join();
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        self.stop();
    }
}
