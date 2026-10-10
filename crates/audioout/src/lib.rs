//! Audio output: calls `fill` on the audio thread whenever the device wants 44.1 kHz stereo s16 frames.

use std::ffi::{c_int, c_void};

type FillFn = unsafe extern "C" fn(user: *mut c_void, out: *mut i16, frames: u32);

extern "C" {
    fn audioout_open(fill: FillFn, user: *mut c_void, err: *mut c_int) -> *mut c_void;
    fn audioout_close(a: *mut c_void);
}

type Callback = Box<dyn FnMut(&mut [i16]) + Send>;

pub struct AudioOut {
    raw: *mut c_void,
    callback: *mut Callback,
}
// The device owns its audio thread; the handle is only used to close it.
unsafe impl Send for AudioOut {}

unsafe extern "C" fn trampoline(user: *mut c_void, out: *mut i16, frames: u32) {
    let cb = &mut *(user as *mut Callback);
    let buf = std::slice::from_raw_parts_mut(out, frames as usize * 2);
    cb(buf);
}

impl AudioOut {
    /// Opens the default output device. `fill` gets an interleaved stereo buffer to fill completely.
    pub fn open(fill: impl FnMut(&mut [i16]) + Send + 'static) -> Result<AudioOut, String> {
        let callback: *mut Callback = Box::into_raw(Box::new(Box::new(fill)));
        let mut err: c_int = 0;
        let raw = unsafe { audioout_open(trampoline, callback as *mut c_void, &mut err) };
        if raw.is_null() {
            unsafe { drop(Box::from_raw(callback)) };
            return Err(format!("no audio output device could be opened (code {err})"));
        }
        Ok(AudioOut { raw, callback })
    }
}

impl Drop for AudioOut {
    fn drop(&mut self) {
        unsafe {
            audioout_close(self.raw); // stops the audio thread before the callback goes away
            drop(Box::from_raw(self.callback));
        }
    }
}
