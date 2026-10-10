//! End-to-end check of the product path: disc image + decoded song + macro + emulation, headless.
//! Needs the user's own disc image and a song; skipped when they are not next to the workspace.
//! Set VIBRIBBON_TEST_DISC / VIBRIBBON_TEST_SONG to use other files.

use macros::Profile;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant};
use vibribbon_player::session::{Event, Params, Session};

/// The machine is a process-wide singleton: tests must not run emulations at the same time.
static MACHINE: Mutex<()> = Mutex::new(());

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..")
}

fn find(env: &str, default: &str) -> Option<PathBuf> {
    let p = std::env::var_os(env).map(PathBuf::from).unwrap_or_else(|| root().join(default));
    p.exists().then_some(p)
}

fn run_macro(name: &str, song: Option<&PathBuf>, until_frame: u32) -> Vec<Event> {
    let _guard = MACHINE.lock().unwrap_or_else(|e| e.into_inner());
    let disc = find("VIBRIBBON_TEST_DISC", "Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue").expect("disc");
    let game = media::open_disc(&disc).unwrap().into_disc().unwrap();
    let profile = Profile::for_boot_name(&game.boot_name().unwrap()).expect("profile");
    let mdef = profile.macros[name].clone();
    let song_disc = song.map(|p| {
        let s = media::decode_song(p).unwrap();
        vibrt::Disc::from_pcm(&s.pcm, &s.title)
    });
    let mut session = Session::start(Params {
        game,
        song: song_disc,
        macro_def: Some(mdef),
        pal: profile.disc.pal,
        headless: true,
        on_frame: None,
    });
    let mut events = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(120);
    while Instant::now() < deadline {
        events.extend(session.shared.take_events());
        let done = events.iter().any(|e| matches!(e, Event::MacroDone { .. } | Event::MacroFailed { .. }));
        if done
            || session.shared.frame_no.load(std::sync::atomic::Ordering::Relaxed) > until_frame
            || session.is_finished()
        {
            break;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    session.stop();
    events.extend(session.shared.take_events());
    events
}

#[test]
fn song_macro_starts_the_song() {
    let (Some(_), Some(song)) = (
        find("VIBRIBBON_TEST_DISC", "Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue"),
        find("VIBRIBBON_TEST_SONG", "Got Your Six.flac"),
    ) else {
        eprintln!("skipped: no disc image / song");
        return;
    };
    let events = run_macro("song", Some(&song), 3000);
    let done = events.iter().find_map(|e| match e {
        Event::MacroDone { frame } => Some(*frame),
        _ => None,
    });
    // the timeline is deterministic: any change here means the emulation (or the macro) changed behaviour
    assert_eq!(done, Some(795), "events: {events:?}");
}

#[test]
fn tutorial_macro_reaches_the_tutorial() {
    if find("VIBRIBBON_TEST_DISC", "Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue").is_none() {
        eprintln!("skipped: no disc image");
        return;
    }
    let events = run_macro("how_to_play", None, 5000);
    assert!(events.iter().any(|e| matches!(e, Event::MacroDone { frame: 3300 })), "events: {events:?}");
}
