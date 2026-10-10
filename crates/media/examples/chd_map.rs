//! Maps every frame of a CHD to the sector of a cue/bin image it equals (to learn the CHD's layout).
use chd::Chd;
use std::collections::HashMap;
use std::fs::File;
use std::io::BufReader;
use std::path::Path;
fn h(b: &[u8]) -> u64 {
    let mut x = 0xcbf29ce484222325u64;
    for &v in b {
        x ^= v as u64;
        x = x.wrapping_mul(0x100000001b3);
    }
    x
}
fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut cue = media::open_disc(Path::new(&args[0])).unwrap();
    let mut sectors: HashMap<u64, Vec<u32>> = HashMap::new();
    let mut buf = [0u8; 2352];
    for lba in 0..cue.total_sectors {
        if cue.read_sector(lba, &mut buf) {
            // skip all-zero sectors: they match everywhere
            if buf.iter().any(|&b| b != 0) {
                sectors.entry(h(&buf)).or_default().push(lba);
            }
        }
    }
    let mut chd = Chd::open(BufReader::new(File::open(&args[1]).unwrap()), None).unwrap();
    let hunk_size = chd.header().hunk_size() as usize;
    let fph = hunk_size / 2448;
    let mut out = chd.get_hunksized_buffer();
    let mut tmp = Vec::new();
    let mut prev: Option<(u32, u32)> = None; // (chd frame, lba) of previous mapped frame
    let mut run_start: Option<(u32, u32)> = None;
    println!("chd frame -> bin sector runs (swapped audio):");
    for hn in 0..chd.header().hunk_count() {
        chd.hunk(hn).unwrap().read_hunk_in(&mut tmp, &mut out).unwrap();
        for i in 0..fph {
            let frame = hn * fph as u32 + i as u32;
            let raw = &out[i * 2448..i * 2448 + 2352];
            let mut a = raw.to_vec();
            let cand = |b: &[u8]| sectors.get(&h(b)).and_then(|v| if v.len() == 1 { Some(v[0]) } else { None });
            let mut sw = a.clone();
            for p in sw.chunks_exact_mut(2) {
                p.swap(0, 1);
            }
            let lba = cand(&a).or_else(|| cand(&sw));
            a.clear();
            match (lba, prev) {
                (Some(l), Some((pf, pl))) if frame == pf + 1 && l == pl + 1 => prev = Some((frame, l)),
                (Some(l), _) => {
                    if let Some((rf, rl)) = run_start {
                        let (pf, pl) = prev.unwrap();
                        println!("  frames {rf}..={pf} -> sectors {rl}..={pl}");
                    }
                    run_start = Some((frame, l));
                    prev = Some((frame, l));
                }
                (None, _) => {}
            }
        }
    }
    if let (Some((rf, rl)), Some((pf, pl))) = (run_start, prev) {
        println!("  frames {rf}..={pf} -> sectors {rl}..={pl}");
    }
}
