//! Checks that the game runs at real-time speed when locked to the audio device:
//! `cargo run --release -p vibribbon-player --example pacing -- game.cue song.flac`
//! (plays the song through the real audio device at volume 0).
use macros::Profile;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};
use vibribbon_player::session::{Event, Params, Session, FLUSH_AUDIO, MUTE};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let game = media::open_disc(std::path::Path::new(&args[0])).unwrap().into_disc().unwrap();
    let profile = Profile::for_boot_name(&game.boot_name().unwrap()).unwrap();
    let song = media::decode_song(std::path::Path::new(&args[1])).unwrap();
    let song_disc = vibrt::Disc::from_pcm(&song.pcm, &song.title);
    let underruns = std::sync::Arc::new(std::sync::atomic::AtomicU32::new(0));
    let (u2, pulled) = (underruns.clone(), std::sync::Arc::new(std::sync::atomic::AtomicU64::new(0)));
    let p2 = pulled.clone();
    let _audio = audioout::AudioOut::open(move |buf| {
        if FLUSH_AUDIO.swap(false, Ordering::Relaxed) {
            vibrt::audio::clear();
        }
        let n = vibrt::audio::pull(buf);
        if n < buf.len() / 2 && !MUTE.load(Ordering::Relaxed) {
            u2.fetch_add(1, Ordering::Relaxed);
        }
        buf[n * 2..].fill(0);
        buf.fill(0); // volume 0: this is a timing test
        p2.fetch_add(n as u64, Ordering::Relaxed);
    })
    .expect("audio device");
    let mut session = Session::start(Params {
        game,
        song: Some(song_disc),
        macro_def: Some(profile.macros["song"].clone()),
        pal: profile.disc.pal,
        headless: false,
        on_frame: None,
    });
    let mut t0: Option<(Instant, u32, u32)> = None;
    let mut pulled0 = 0u64;
    let start = Instant::now();
    loop {
        for ev in session.shared.take_events() {
            if let Event::MacroDone { frame } = ev {
                println!("macro done at frame {frame} after {:.1?}", start.elapsed());
                t0 = Some((Instant::now(), frame, underruns.load(Ordering::Relaxed)));
                pulled0 = pulled.load(Ordering::Relaxed);
            }
        }
        if let Some((t, f0, u0)) = t0 {
            if t.elapsed() > Duration::from_secs(15) {
                let f = session.shared.frame_no.load(Ordering::Relaxed);
                let secs = t.elapsed().as_secs_f64();
                let pf = pulled.load(Ordering::Relaxed) - pulled0;
                println!(
                    "{} frames in {:.2} s = {:.2} fps (PAL is 49.8); audio underruns during play: {}",
                    f - f0,
                    secs,
                    (f - f0) as f64 / secs,
                    underruns.load(Ordering::Relaxed) - u0
                );
                println!(
                    "audio frames pulled: {} = {:.0} per second (44100 expected), {:.1} audio frames per video frame",
                    pf,
                    pf as f64 / secs,
                    pf as f64 / (f - f0) as f64
                );
                break;
            }
        }
        if start.elapsed() > Duration::from_secs(60) {
            println!("timeout");
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    session.stop();
}
