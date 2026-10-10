//! `cargo run --release --example cmp_discs -- a.cue b.chd` : compare two disc images sector by sector.
use std::path::Path;
fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut a = media::open_disc(Path::new(&args[0])).expect("open a");
    let mut b = media::open_disc(Path::new(&args[1])).expect("open b");
    println!("a: {} tracks, {} sectors", a.tracks.len(), a.total_sectors);
    println!("b: {} tracks, {} sectors", b.tracks.len(), b.total_sectors);
    for (x, y) in a.tracks.iter().zip(&b.tracks) {
        let same = x.audio == y.audio && x.start_lba == y.start_lba && x.length == y.length;
        println!(
            "track {}: a start {} len {} | b start {} len {} {}",
            x.number,
            x.start_lba,
            x.length,
            y.start_lba,
            y.length,
            if same { "ok" } else { "(different table of contents)" }
        );
    }
    let total = a.total_sectors.min(b.total_sectors);
    let (mut bad, mut first_bad) = (0u32, None);
    let (mut ba, mut bb) = ([0u8; 2352], [0u8; 2352]);
    for lba in 0..total {
        let ra = a.read_sector(lba, &mut ba);
        let rb = b.read_sector(lba, &mut bb);
        if ra != rb || (ra && ba != bb) {
            bad += 1;
            first_bad.get_or_insert(lba);
        }
    }
    println!(
        "compared {total} sectors: {bad} differ{}",
        first_bad.map(|l| format!(" (first at {l})")).unwrap_or_default()
    );
    std::process::exit(if bad == 0 && a.total_sectors == b.total_sectors { 0 } else { 1 });
}
