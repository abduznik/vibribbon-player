//! Disc images and songs for the vibrecomp player.

use std::path::{Path, PathBuf};
use vibrt::{Disc, SectorSource, Track};

mod chd;
mod cue;
mod song;

pub use song::{decode as decode_song, Song, SongError};

#[derive(Debug)]
pub enum DiscError {
    Io(PathBuf, std::io::Error),
    Missing(PathBuf),
    Format(String),
    Unsupported(String),
}

impl std::fmt::Display for DiscError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DiscError::Io(p, e) => write!(f, "cannot read {}: {}", p.display(), e),
            DiscError::Missing(p) => write!(f, "the disc image refers to a file that is missing: {}", p.display()),
            DiscError::Format(m) => write!(f, "unrecognised disc image: {m}"),
            DiscError::Unsupported(m) => write!(f, "unsupported disc image: {m}"),
        }
    }
}
impl std::error::Error for DiscError {}

/// An opened disc image: sector source plus table of contents. Convert it with [`DiscImage::into_disc`].
pub struct DiscImage {
    source: Box<dyn SectorSource>,
    pub tracks: Vec<Track>,
    pub total_sectors: u32,
    pub label: String,
}

impl DiscImage {
    pub(crate) fn new(source: Box<dyn SectorSource>, tracks: Vec<Track>, total_sectors: u32, label: String) -> Self {
        DiscImage { source, tracks, total_sectors, label }
    }

    /// Reads one raw sector (absolute LBA). Mainly for tools and tests.
    pub fn read_sector(&mut self, lba: u32, out: &mut [u8; 2352]) -> bool {
        self.source.read_sector(lba, out)
    }

    pub fn into_disc(self) -> Option<Disc> {
        struct Boxed(Box<dyn SectorSource>);
        impl SectorSource for Boxed {
            fn read_sector(&mut self, lba: u32, out: &mut [u8; 2352]) -> bool {
                self.0.read_sector(lba, out)
            }
        }
        Disc::from_source(Boxed(self.source), &self.tracks, self.total_sectors, &self.label)
    }
}

/// Opens a disc image by file extension (`.cue` + `.bin` files, or `.chd`).
pub fn open_disc(path: &Path) -> Result<DiscImage, DiscError> {
    let ext = path.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    match ext.as_str() {
        "cue" => cue::open(path),
        "chd" => chd::open(path),
        other => Err(DiscError::Unsupported(format!("`.{other}` files (use the .cue file)"))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Needs the user's own disc image next to the workspace; skipped when absent.
    #[test]
    fn reads_vib_ribbon_cue() {
        let cue = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue");
        if !cue.exists() {
            eprintln!("skipped: no disc image");
            return;
        }
        let img = open_disc(&cue).expect("open");
        assert_eq!(img.tracks.len(), 8);
        assert!(!img.tracks[0].audio && img.tracks[1].audio);
        let disc = img.into_disc().expect("disc");
        assert_eq!(disc.boot_name().as_deref(), Some("SCES_028.73"));
        assert!(disc.has_file("MAIN_G.EXE"));
        assert!(!disc.has_file("NOPE.EXE"));
    }

    /// Makes a CHD of the user's disc with chdman and checks every sector and the track table against the
    /// BIN/CUE reader. Slow (a few seconds); only runs with VIBRECOMP_SLOW_TESTS=1 and chdman installed.
    #[test]
    fn chd_equals_cue() {
        let cue = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue");
        if std::env::var_os("VIBRECOMP_SLOW_TESTS").is_none() || !cue.exists() {
            eprintln!("skipped");
            return;
        }
        let chd = std::env::temp_dir().join("vibrecomp_test.chd");
        let ok = std::process::Command::new("chdman")
            .args(["createcd", "-f", "-i"])
            .arg(&cue)
            .arg("-o")
            .arg(&chd)
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false);
        if !ok {
            eprintln!("skipped: chdman not available");
            return;
        }
        let mut a = open_disc(&cue).unwrap();
        let mut b = open_disc(&chd).unwrap();
        assert_eq!(a.total_sectors, b.total_sectors);
        for (x, y) in a.tracks.iter().zip(&b.tracks) {
            assert_eq!((x.audio, x.start_lba), (y.audio, y.start_lba), "track {}", x.number);
        }
        let (mut ba, mut bb) = ([0u8; 2352], [0u8; 2352]);
        for lba in 0..a.total_sectors {
            assert!(a.read_sector(lba, &mut ba) && b.read_sector(lba, &mut bb), "read {lba}");
            assert!(ba == bb, "sector {lba} differs");
        }
        let _ = std::fs::remove_file(chd);
    }

    #[test]
    fn decodes_flac_when_present() {
        let f = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../Got Your Six.flac");
        if !f.exists() {
            eprintln!("skipped: no song");
            return;
        }
        let song = decode_song(&f).expect("decode");
        assert!(song.seconds() > 30.0, "song is {} s", song.seconds());
        assert!(song.pcm.iter().any(|&s| s.abs() > 1000));
    }

    #[test]
    fn decodes_and_resamples_wav() {
        // 1 s of 48 kHz stereo sine written by hand
        let dir = std::env::temp_dir().join("vibrecomp_media_test.wav");
        let sr = 48_000u32;
        let n = sr as usize;
        let mut data = Vec::new();
        for i in 0..n {
            let v = ((i as f32 * 440.0 * std::f32::consts::TAU / sr as f32).sin() * 12000.0) as i16;
            data.extend_from_slice(&v.to_le_bytes());
            data.extend_from_slice(&v.to_le_bytes());
        }
        let mut wav = Vec::new();
        wav.extend_from_slice(b"RIFF");
        wav.extend_from_slice(&(36 + data.len() as u32).to_le_bytes());
        wav.extend_from_slice(b"WAVEfmt ");
        wav.extend_from_slice(&16u32.to_le_bytes());
        wav.extend_from_slice(&1u16.to_le_bytes());
        wav.extend_from_slice(&2u16.to_le_bytes());
        wav.extend_from_slice(&sr.to_le_bytes());
        wav.extend_from_slice(&(sr * 4).to_le_bytes());
        wav.extend_from_slice(&4u16.to_le_bytes());
        wav.extend_from_slice(&16u16.to_le_bytes());
        wav.extend_from_slice(b"data");
        wav.extend_from_slice(&(data.len() as u32).to_le_bytes());
        wav.extend_from_slice(&data);
        std::fs::write(&dir, wav).unwrap();
        let song = decode_song(&dir).expect("decode wav");
        let frames = song.pcm.len() / 2;
        assert!((frames as i64 - 44_100).abs() < 8, "got {frames} frames");
        let peak = song.pcm.iter().map(|s| s.abs()).max().unwrap();
        assert!(peak > 11000 && peak < 13000, "peak {peak}");
    }
}
