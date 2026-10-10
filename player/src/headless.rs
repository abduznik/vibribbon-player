//! `--headless`: run a macro without a window (regression tests, dumping frames).
//!
//!   vibribbon-player --headless --disc game.cue [--song song.flac] [--macro song] [--frames N]
//!                    [--dump DIR EVERY]

use crate::session::{Event, Frame, Params, Session};
use macros::Profile;
use std::path::PathBuf;
use std::time::{Duration, Instant};

pub fn run(args: &[String]) -> i32 {
    let mut disc = None;
    let mut song = None;
    let mut macro_name = None;
    let mut frames = 6000u32;
    let mut dump: Option<(PathBuf, u32)> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--disc" => {
                i += 1;
                disc = args.get(i).cloned()
            }
            "--song" => {
                i += 1;
                song = args.get(i).cloned()
            }
            "--macro" => {
                i += 1;
                macro_name = args.get(i).cloned()
            }
            "--frames" => {
                i += 1;
                frames = args.get(i).and_then(|v| v.parse().ok()).unwrap_or(frames)
            }
            "--dump" => {
                dump = Some((
                    PathBuf::from(args.get(i + 1).cloned().unwrap_or_default()),
                    args.get(i + 2).and_then(|v| v.parse().ok()).unwrap_or(50),
                ));
                i += 2;
            }
            _ => {}
        }
        i += 1;
    }
    let Some(disc_path) = disc else {
        eprintln!("usage: --headless --disc game.cue [--song file] [--macro name] [--frames N] [--dump DIR EVERY]");
        return 2;
    };
    let game = match media::open_disc(std::path::Path::new(&disc_path)).map(|d| d.into_disc()) {
        Ok(Some(d)) => d,
        Ok(None) => {
            eprintln!("cannot create disc");
            return 1;
        }
        Err(e) => {
            eprintln!("{e}");
            return 1;
        }
    };
    let boot = game.boot_name().unwrap_or_default();
    let Some(profile) = Profile::for_boot_name(&boot) else {
        eprintln!("no macros for this disc (boot executable '{boot}')");
        return 1;
    };
    let name = macro_name.unwrap_or_else(|| "song".into());
    let Some(mdef) = profile.macros.get(&name).cloned() else {
        eprintln!("no macro '{name}'; available: {:?}", profile.macros.keys().collect::<Vec<_>>());
        return 1;
    };
    let song_disc = if mdef.needs_song {
        let Some(path) = song else {
            eprintln!("macro '{name}' needs --song");
            return 2;
        };
        match media::decode_song(std::path::Path::new(&path)) {
            Ok(s) => {
                eprintln!("song: {} ({:.1} s)", s.title, s.seconds());
                Some(vibrt::Disc::from_pcm(&s.pcm, &s.title))
            }
            Err(e) => {
                eprintln!("{e}");
                return 1;
            }
        }
    } else {
        None
    };

    if let Some((dir, _)) = &dump {
        let _ = std::fs::create_dir_all(dir);
    }
    let on_frame: Option<Box<dyn FnMut(u32, &Frame) + Send>> = dump.map(|(dir, every)| {
        Box::new(move |n: u32, f: &Frame| {
            if n % every != 0 {
                return;
            }
            let mut out = format!("P6\n{} {}\n255\n", f.w, f.h).into_bytes();
            for y in 0..f.h {
                for x in 0..f.w {
                    let p = f.rgba[y * 1024 + x];
                    out.extend_from_slice(&[p as u8, (p >> 8) as u8, (p >> 16) as u8]);
                }
            }
            let _ = std::fs::write(dir.join(format!("frame{n:05}.ppm")), out);
        }) as Box<dyn FnMut(u32, &Frame) + Send>
    });

    eprintln!(
        "macro '{}' ({}), boot {}",
        name,
        mdef.title,
        if mdef.boot_exe.is_some() { mdef.boot_exe.clone().unwrap() } else { boot }
    );
    let mut session = Session::start(Params {
        game,
        song: song_disc,
        macro_def: Some(mdef),
        pal: profile.disc.pal,
        headless: true,
        on_frame,
    });
    let started = Instant::now();
    let mut code = 3;
    let mut stop_at = frames;
    loop {
        for ev in session.shared.take_events() {
            match ev {
                Event::MacroDone { frame } => {
                    println!("macro done at frame {frame}");
                    code = 0;
                    stop_at = stop_at.min(frame + 300); // keep going a little so the start of play is dumped
                }
                Event::MacroFailed { frame, reason } => {
                    println!("macro FAILED at frame {frame}: {reason}");
                    code = 1;
                    stop_at = stop_at.min(frame + 60);
                }
                Event::Exited { frames } => println!("game exited after {frames} frames"),
            }
        }
        if session.shared.frame_no.load(std::sync::atomic::Ordering::Relaxed) >= stop_at || session.is_finished() {
            break;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    session.stop();
    for ev in session.shared.take_events() {
        if let Event::Exited { frames } = ev {
            println!("stopped after {frames} frames in {:.1?}", started.elapsed())
        }
    }
    code
}
