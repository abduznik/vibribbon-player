//! `cargo run --example decode -- song.mp3` : decode a song and print what came out.
fn main() {
    let path = std::env::args().nth(1).expect("usage: decode <file>");
    let t = std::time::Instant::now();
    match media::decode_song(std::path::Path::new(&path)) {
        Ok(s) => {
            let peak = s.pcm.iter().map(|v| v.unsigned_abs()).max().unwrap_or(0);
            println!("{}: {:.1} s, peak {}, decoded in {:.2?}", s.title, s.seconds(), peak, t.elapsed());
        }
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    }
}
