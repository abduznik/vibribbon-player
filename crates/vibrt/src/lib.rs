//! Safe Rust API over the C core (`runtime/vibrt.h`).
//!
//! The core keeps one machine in process-wide state, so only one [`Emulator`] can run at a time; this is
//! enforced at run time.

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};

mod ffi {
    use super::*;

    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct VibrtTrack {
        pub number: i32,
        pub audio: i32,
        pub start_lba: u32,
        pub length: u32,
    }

    pub type ReadSectorFn = unsafe extern "C" fn(user: *mut c_void, lba: u32, out: *mut u8) -> c_int;

    #[repr(C)]
    pub struct VibrtCallbacks {
        pub frame: Option<unsafe extern "C" fn(user: *mut c_void, frame_no: u32)>,
        pub cd_command: Option<unsafe extern "C" fn(user: *mut c_void, cmd: u8)>,
    }

    pub enum Disc {}

    extern "C" {
        pub fn vibrt_disc_new(
            tracks: *const VibrtTrack,
            ntracks: c_int,
            total_sectors: u32,
            read: ReadSectorFn,
            user: *mut c_void,
            destroy_user: Option<unsafe extern "C" fn(*mut c_void)>,
            label: *const c_char,
        ) -> *mut Disc;
        pub fn vibrt_disc_from_pcm(pcm: *const i16, frames: usize, label: *const c_char) -> *mut Disc;
        pub fn vibrt_disc_free(d: *mut Disc);
        pub fn vibrt_disc_boot_name(d: *mut Disc, out: *mut c_char, n: usize) -> c_int;
        pub fn vibrt_disc_has_file(d: *mut Disc, path: *const c_char) -> c_int;
        pub fn vibrt_set_callbacks(cb: *const VibrtCallbacks, user: *mut c_void);
        pub fn vibrt_run(game: *mut Disc, pal: c_int, boot_exe: *const c_char) -> c_int;
        pub fn vibrt_request_stop();
        pub fn vibrt_stop_requested() -> c_int;
        pub fn vibrt_set_pad(pressed: u16);
        pub fn vibrt_swap_disc(d: *mut Disc, open_ms: u32);
        pub fn vibrt_set_fast_forward(on: c_int);
        pub fn vibrt_set_cd_turbo(on: c_int);
        pub fn vibrt_set_verbose(level: c_int);
        pub fn vibrt_set_pacing(on: c_int);
        pub fn vibrt_selftest() -> c_int;
        pub fn vibrt_audio_fill() -> usize;
        pub fn vibrt_audio_pull(dst: *mut i16, frames: usize) -> usize;
        pub fn vibrt_audio_clear();
        pub fn vibrt_render_frame(rgba: *mut u32, stride: usize, w: *mut c_int, h: *mut c_int) -> c_int;
    }
}

/// One track of a disc image.
#[derive(Clone, Copy, Debug)]
pub struct Track {
    pub number: i32,
    pub audio: bool,
    /// Absolute LBA of INDEX 01.
    pub start_lba: u32,
    /// Length in sectors.
    pub length: u32,
}

/// Source of raw 2352-byte sectors.
pub trait SectorSource: Send + 'static {
    /// Reads the sector at absolute `lba` into `out`. Returns false when it cannot be read.
    fn read_sector(&mut self, lba: u32, out: &mut [u8; 2352]) -> bool;
}

/// A disc handed to the core. Dropping it frees it; it must not be dropped while the emulator uses it
/// (the [`Emulator`] holds on to the discs it was given, so this is automatic).
pub struct Disc {
    ptr: *mut ffi::Disc,
}
// The core only touches discs from the emulation thread.
unsafe impl Send for Disc {}

unsafe extern "C" fn read_trampoline(user: *mut c_void, lba: u32, out: *mut u8) -> c_int {
    let src = &mut *(user as *mut Box<dyn SectorSource>);
    let buf = &mut *(out as *mut [u8; 2352]);
    if src.read_sector(lba, buf) {
        0
    } else {
        -1
    }
}
unsafe extern "C" fn destroy_trampoline(user: *mut c_void) {
    drop(Box::from_raw(user as *mut Box<dyn SectorSource>));
}

impl Disc {
    /// Builds a disc from a sector source and its table of contents.
    pub fn from_source(source: impl SectorSource, tracks: &[Track], total_sectors: u32, label: &str) -> Option<Disc> {
        let ffi_tracks: Vec<ffi::VibrtTrack> = tracks
            .iter()
            .map(|t| ffi::VibrtTrack {
                number: t.number,
                audio: t.audio as i32,
                start_lba: t.start_lba,
                length: t.length,
            })
            .collect();
        let boxed: Box<Box<dyn SectorSource>> = Box::new(Box::new(source));
        let user = Box::into_raw(boxed) as *mut c_void;
        let label = CString::new(label).ok()?;
        let ptr = unsafe {
            ffi::vibrt_disc_new(
                ffi_tracks.as_ptr(),
                ffi_tracks.len() as c_int,
                total_sectors,
                read_trampoline,
                user,
                Some(destroy_trampoline),
                label.as_ptr(),
            )
        };
        if ptr.is_null() {
            unsafe { destroy_trampoline(user) };
            return None;
        }
        Some(Disc { ptr })
    }

    /// A one-track audio CD from 44.1 kHz interleaved stereo samples.
    pub fn from_pcm(interleaved: &[i16], label: &str) -> Disc {
        let label = CString::new(label).unwrap_or_default();
        let ptr = unsafe { ffi::vibrt_disc_from_pcm(interleaved.as_ptr(), interleaved.len() / 2, label.as_ptr()) };
        Disc { ptr }
    }

    /// The executable named by SYSTEM.CNF, e.g. `SCES_028.73`.
    pub fn boot_name(&self) -> Option<String> {
        let mut buf = [0 as c_char; 64];
        if unsafe { ffi::vibrt_disc_boot_name(self.ptr, buf.as_mut_ptr(), buf.len()) } == 0 {
            return None;
        }
        Some(unsafe { CStr::from_ptr(buf.as_ptr()) }.to_string_lossy().into_owned())
    }

    pub fn has_file(&self, path: &str) -> bool {
        match CString::new(path) {
            Ok(p) => unsafe { ffi::vibrt_disc_has_file(self.ptr, p.as_ptr()) != 0 },
            Err(_) => false,
        }
    }
}

impl Drop for Disc {
    fn drop(&mut self) {
        unsafe { ffi::vibrt_disc_free(self.ptr) }
    }
}

/// Controller buttons (bit set = pressed).
pub mod pad {
    pub const SELECT: u16 = 0x0001;
    pub const START: u16 = 0x0008;
    pub const UP: u16 = 0x0010;
    pub const RIGHT: u16 = 0x0020;
    pub const DOWN: u16 = 0x0040;
    pub const LEFT: u16 = 0x0080;
    pub const L2: u16 = 0x0100;
    pub const R2: u16 = 0x0200;
    pub const L1: u16 = 0x0400;
    pub const R1: u16 = 0x0800;
    pub const TRIANGLE: u16 = 0x1000;
    pub const CIRCLE: u16 = 0x2000;
    pub const CROSS: u16 = 0x4000;
    pub const SQUARE: u16 = 0x8000;
}

/// CD-ROM command bytes a macro may want to wait for.
pub mod cdcmd {
    pub const SETLOC: u8 = 0x02;
    pub const PLAY: u8 = 0x03;
    pub const READN: u8 = 0x06;
    pub const MUTE: u8 = 0x0b;
    pub const DEMUTE: u8 = 0x0c;
    pub const SEEKP: u8 = 0x16;
    pub const GETLOCP: u8 = 0x11;
}

/// What the emulation thread reports back (all methods run on the emulation thread).
pub trait Host: Send {
    /// Once per vblank, before the game polls the pad. Use `ctl` to set the pad, swap discs, render the frame.
    fn frame(&mut self, ctl: &mut Control<'_>, frame_no: u32);
    /// The game sent a command to the CD drive.
    fn cd_command(&mut self, _ctl: &mut Control<'_>, _cmd: u8) {}
}

/// Handle given to host callbacks to steer the running machine.
pub struct Control<'a> {
    extra: &'a [*mut ffi::Disc],
}

impl Control<'_> {
    pub fn set_pad(&mut self, pressed: u16) {
        unsafe { ffi::vibrt_set_pad(pressed) }
    }
    pub fn set_fast_forward(&mut self, on: bool) {
        unsafe { ffi::vibrt_set_fast_forward(on as c_int) }
    }
    pub fn set_cd_turbo(&mut self, on: bool) {
        unsafe { ffi::vibrt_set_cd_turbo(on as c_int) }
    }
    /// Swaps in the `index`-th extra disc given to [`Emulator::new`] (lid open for `open_ms` of emulated time).
    pub fn swap_disc(&mut self, index: usize, open_ms: u32) {
        if let Some(&d) = self.extra.get(index) {
            unsafe { ffi::vibrt_swap_disc(d, open_ms) }
        }
    }
    /// Copies the current picture as RGBA into `rgba` (needs 1024*512 entries); returns (width, height) and
    /// whether the display was enabled.
    pub fn render_frame(&mut self, rgba: &mut [u32]) -> (usize, usize, bool) {
        assert!(rgba.len() >= 1024 * 512);
        let (mut w, mut h) = (0 as c_int, 0 as c_int);
        let on = unsafe { ffi::vibrt_render_frame(rgba.as_mut_ptr(), 1024, &mut w, &mut h) };
        (w as usize, h as usize, on != 0)
    }
}

struct Shared<'a> {
    host: &'a mut dyn Host,
    extra: Vec<*mut ffi::Disc>,
}

unsafe extern "C" fn frame_trampoline(user: *mut c_void, frame_no: u32) {
    let shared = &mut *(user as *mut Shared);
    let mut ctl = Control { extra: &shared.extra };
    shared.host.frame(&mut ctl, frame_no);
}
unsafe extern "C" fn cd_trampoline(user: *mut c_void, cmd: u8) {
    let shared = &mut *(user as *mut Shared);
    let mut ctl = Control { extra: &shared.extra };
    shared.host.cd_command(&mut ctl, cmd);
}

static RUNNING: AtomicBool = AtomicBool::new(false);

/// Runs a game. Create it on the emulation thread (or move it there) and call [`Emulator::run`].
pub struct Emulator {
    game: Disc,
    keep_alive: Vec<Disc>,
}

impl Emulator {
    /// `extra` discs (e.g. the song) are kept alive for as long as the emulator exists.
    pub fn new(game: Disc, extra: Vec<Disc>) -> Emulator {
        Emulator { game, keep_alive: extra }
    }

    /// Runs until the game exits or [`stop`] is called from any thread. Returns the number of frames run.
    /// Panics if another emulator is running.
    pub fn run(&mut self, pal: bool, boot_exe: Option<&str>, host: &mut dyn Host) -> u32 {
        assert!(!RUNNING.swap(true, Ordering::SeqCst), "only one emulator can run at a time");
        let mut shared = Shared { host, extra: self.keep_alive.iter().map(|d| d.ptr).collect() };
        let cb = ffi::VibrtCallbacks { frame: Some(frame_trampoline), cd_command: Some(cd_trampoline) };
        let boot = boot_exe.map(|b| CString::new(b).unwrap());
        let frames = unsafe {
            ffi::vibrt_set_callbacks(&cb, &mut shared as *mut Shared as *mut c_void);
            let f = ffi::vibrt_run(self.game.ptr, pal as c_int, boot.as_ref().map_or(std::ptr::null(), |b| b.as_ptr()));
            ffi::vibrt_set_callbacks(std::ptr::null(), std::ptr::null_mut());
            f
        };
        RUNNING.store(false, Ordering::SeqCst);
        frames as u32
    }
}

/// True once [`stop`] has been called and the machine has not been restarted since.
pub fn stop_requested() -> bool {
    unsafe { ffi::vibrt_stop_requested() != 0 }
}

/// Asks the running emulator (if any) to stop; `run` returns shortly after.
pub fn stop() {
    unsafe { ffi::vibrt_request_stop() }
}

pub fn set_verbose(level: i32) {
    unsafe { ffi::vibrt_set_verbose(level) }
}

/// Real-time pacing against the audio output (default on). Turn it off to run as fast as possible.
pub fn set_pacing(on: bool) {
    unsafe { ffi::vibrt_set_pacing(on as c_int) }
}

/// Audio output of the machine: 44.1 kHz stereo s16. Safe to use from the audio thread.
pub mod audio {
    use super::ffi;
    /// Frames currently buffered.
    pub fn buffered() -> usize {
        unsafe { ffi::vibrt_audio_fill() }
    }
    /// Pulls up to `dst.len()/2` frames; returns the number of frames written.
    pub fn pull(dst: &mut [i16]) -> usize {
        unsafe { ffi::vibrt_audio_pull(dst.as_mut_ptr(), dst.len() / 2) }
    }
    pub fn clear() {
        unsafe { ffi::vibrt_audio_clear() }
    }
}

/// Checks that the platform's coroutine switching works (the core's thread scheduler depends on it).
pub fn selftest() -> bool {
    unsafe { ffi::vibrt_selftest() == 0 }
}

#[cfg(test)]
mod tests {
    #[test]
    fn coroutines_switch() {
        assert!(super::selftest());
    }
}
