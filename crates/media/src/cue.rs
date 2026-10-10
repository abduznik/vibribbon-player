//! CUE/BIN disc images (single or multi-file, raw 2352-byte sectors).

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use vibrt::{SectorSource, Track};

use crate::{DiscError, DiscImage};

struct BinFile {
    file: File,
    start_lba: u32,
    sectors: u32,
}

struct CueBin {
    files: Vec<BinFile>,
}

impl SectorSource for CueBin {
    fn read_sector(&mut self, lba: u32, out: &mut [u8; 2352]) -> bool {
        for b in &mut self.files {
            if lba >= b.start_lba && lba < b.start_lba + b.sectors {
                let off = (lba - b.start_lba) as u64 * 2352;
                return b.file.seek(SeekFrom::Start(off)).is_ok() && b.file.read_exact(out).is_ok();
            }
        }
        false
    }
}

fn msf_to_frames(s: &str) -> Option<u32> {
    let mut it = s.split(':').map(|p| p.trim().parse::<u32>().ok());
    let (m, sec, f) = (it.next()??, it.next()??, it.next()??);
    Some((m * 60 + sec) * 75 + f)
}

/// Finds `name` next to the cue file; tolerates different letter case and `\` separators.
fn resolve(dir: &Path, name: &str) -> Option<PathBuf> {
    let name = name.replace('\\', "/");
    let direct = dir.join(&name);
    if direct.is_file() {
        return Some(direct);
    }
    let base = Path::new(&name).file_name()?.to_string_lossy().to_lowercase();
    std::fs::read_dir(dir)
        .ok()?
        .flatten()
        .find(|e| e.file_name().to_string_lossy().to_lowercase() == base)
        .map(|e| e.path())
}

pub fn open(cue_path: &Path) -> Result<DiscImage, DiscError> {
    let text = std::fs::read(cue_path).map_err(|e| DiscError::Io(cue_path.to_path_buf(), e))?;
    let text = String::from_utf8_lossy(&text);
    let dir = cue_path.parent().map(Path::to_path_buf).unwrap_or_default();

    struct T {
        number: i32,
        audio: bool,
        file: usize,
        index1: u32,
    }
    let mut files: Vec<BinFile> = Vec::new();
    let mut tracks: Vec<T> = Vec::new();
    let mut next_lba = 0u32;

    for raw in text.lines() {
        let line = raw.trim();
        let up = line.to_ascii_uppercase();
        if up.starts_with("FILE") {
            let q1 = line.find('"');
            let q2 = q1.and_then(|a| line[a + 1..].find('"').map(|b| a + 1 + b));
            let (Some(a), Some(b)) = (q1, q2) else { continue };
            let name = &line[a + 1..b];
            let path = resolve(&dir, name).ok_or_else(|| DiscError::Missing(dir.join(name)))?;
            let file = File::open(&path).map_err(|e| DiscError::Io(path.clone(), e))?;
            let len = file.metadata().map_err(|e| DiscError::Io(path.clone(), e))?.len();
            let sectors = (len / 2352) as u32;
            files.push(BinFile { file, start_lba: next_lba, sectors });
            next_lba += sectors;
        } else if up.starts_with("TRACK") {
            let mut it = line.split_whitespace().skip(1);
            let number = it.next().and_then(|n| n.parse().ok()).unwrap_or(0);
            let ty = it.next().unwrap_or("").to_ascii_uppercase();
            if files.is_empty() || number < 1 {
                return Err(DiscError::Format("TRACK before FILE in cue sheet".into()));
            }
            tracks.push(T { number, audio: ty.starts_with("AUDIO"), file: files.len() - 1, index1: 0 });
        } else if up.starts_with("INDEX") {
            let mut it = line.split_whitespace().skip(1);
            let idx: u32 = it.next().and_then(|n| n.parse().ok()).unwrap_or(99);
            if idx == 1 {
                if let (Some(t), Some(frames)) = (tracks.last_mut(), it.next().and_then(msf_to_frames)) {
                    t.index1 = frames;
                }
            }
        }
    }
    if tracks.is_empty() {
        return Err(DiscError::Format("no tracks in cue sheet".into()));
    }

    let starts: Vec<u32> = tracks.iter().map(|t| files[t.file].start_lba + t.index1).collect();
    let out: Vec<Track> = tracks
        .iter()
        .enumerate()
        .map(|(i, t)| {
            let end = starts.get(i + 1).copied().unwrap_or(next_lba);
            Track { number: t.number, audio: t.audio, start_lba: starts[i], length: end.saturating_sub(starts[i]) }
        })
        .collect();
    let label = cue_path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "disc".into());
    Ok(DiscImage::new(Box::new(CueBin { files }), out, next_lba, label))
}
