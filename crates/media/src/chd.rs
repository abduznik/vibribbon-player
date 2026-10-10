//! CHD (MAME compressed hunks of data) disc images, as made by `chdman createcd`.
//!
//! Layout (per the CHD CD-ROM convention): every frame is 2352 bytes of sector data plus 96 bytes of subcode;
//! each track's frames are padded up to a multiple of 4 frames; a track's pregap is either the first frames
//! of its stored `FRAMES` (`PGTYPE` starting with `V`) or virtual (silent). CD-DA frames are stored big-endian.
//! Verified against a chdman-made CHD of a real disc: every sector equals the corresponding BIN sector.

use chd::metadata::{KnownMetadata, MetadataTag};
use chd::Chd;
use std::fs::File;
use std::io::BufReader;
use std::path::Path;
use vibrt::{SectorSource, Track};

use crate::{DiscError, DiscImage};

const FRAME_BYTES: usize = 2352 + 96;
const TRACK_ALIGN: u32 = 4;
/// The disc's first data sector (MSF 00:02:00) is LBA 150 in the disc-wide numbering used below; the core
/// numbers sectors from there.
const LEAD_IN: u32 = 150;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct TrackMeta {
    pub number: i32,
    pub mode: String,
    pub frames: u32,
    pub pregap: u32,
    pub pregap_in_file: bool,
}

/// One contiguous run of sectors: `file_frame` is None for a silent (virtual) pregap.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Run {
    pub disc_start: u32,
    pub len: u32,
    pub file_frame: Option<u32>,
    pub audio: bool,
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct Layout {
    pub runs: Vec<Run>,
    /// (track number, audio, first sector of INDEX 01 in core numbering, sectors)
    pub tracks: Vec<(i32, bool, u32, u32)>,
    pub total: u32,
}

/// Parses one `CHT2`/`CHTR` metadata string.
pub(crate) fn parse_track(text: &str) -> Option<TrackMeta> {
    let (mut number, mut mode, mut frames, mut pregap, mut pgtype) = (None, None, None, 0u32, String::new());
    for field in text.trim_end_matches('\0').split_whitespace() {
        let (k, v) = field.split_once(':')?;
        match k {
            "TRACK" => number = v.parse().ok(),
            "TYPE" => mode = Some(v.to_string()),
            "FRAMES" => frames = v.parse().ok(),
            "PREGAP" => pregap = v.parse().unwrap_or(0),
            "PGTYPE" => pgtype = v.to_string(),
            _ => {}
        }
    }
    Some(TrackMeta {
        number: number?,
        mode: mode?,
        frames: frames?,
        pregap,
        pregap_in_file: pregap > 0 && pgtype.starts_with('V'),
    })
}

pub(crate) fn layout(tracks: &[TrackMeta]) -> Layout {
    let mut runs = Vec::new();
    let mut out = Vec::new();
    let mut disc = 0u32;
    let mut file = 0u32;
    for t in tracks {
        let audio = t.mode == "AUDIO";
        if t.pregap_in_file && t.pregap < t.frames {
            // the pregap is the first `pregap` frames of the track's FRAMES
            runs.push(Run { disc_start: disc, len: t.pregap, file_frame: Some(file), audio });
            runs.push(Run {
                disc_start: disc + t.pregap,
                len: t.frames - t.pregap,
                file_frame: Some(file + t.pregap),
                audio,
            });
            out.push((t.number, audio, (disc + t.pregap).saturating_sub(LEAD_IN), t.frames - t.pregap));
        } else {
            // a pregap that is not stored is silence; a data track always has a two second one
            let pregap = if t.pregap == 0 && !audio { LEAD_IN } else { t.pregap };
            if pregap > 0 {
                runs.push(Run { disc_start: disc, len: pregap, file_frame: None, audio });
                disc += pregap;
            }
            runs.push(Run { disc_start: disc, len: t.frames, file_frame: Some(file), audio });
            out.push((t.number, audio, disc.saturating_sub(LEAD_IN), t.frames));
        }
        disc += t.frames;
        file += t.frames;
        file = file.div_ceil(TRACK_ALIGN) * TRACK_ALIGN;
    }
    Layout { runs, tracks: out, total: disc.saturating_sub(LEAD_IN) }
}

struct ChdSource {
    chd: Chd<BufReader<File>>,
    runs: Vec<Run>,
    frames_per_hunk: u32,
    hunk: Vec<u8>,
    scratch: Vec<u8>,
    loaded: Option<u32>,
}

impl SectorSource for ChdSource {
    fn read_sector(&mut self, lba: u32, out: &mut [u8; 2352]) -> bool {
        let d = lba + LEAD_IN;
        let Some(run) = self.runs.iter().find(|r| d >= r.disc_start && d < r.disc_start + r.len) else {
            return false;
        };
        let Some(base) = run.file_frame else {
            out.fill(0);
            return true;
        };
        let frame = base + (d - run.disc_start);
        let hunk_no = frame / self.frames_per_hunk;
        if self.loaded != Some(hunk_no) {
            let ok = self.chd.hunk(hunk_no).and_then(|mut h| h.read_hunk_in(&mut self.scratch, &mut self.hunk)).is_ok();
            if !ok {
                self.loaded = None;
                return false;
            }
            self.loaded = Some(hunk_no);
        }
        let off = (frame % self.frames_per_hunk) as usize * FRAME_BYTES;
        out.copy_from_slice(&self.hunk[off..off + 2352]);
        if run.audio {
            for pair in out.chunks_exact_mut(2) {
                pair.swap(0, 1);
            }
        }
        true
    }
}

pub fn open(path: &Path) -> Result<DiscImage, DiscError> {
    let io = |e: std::io::Error| DiscError::Io(path.to_path_buf(), e);
    let bad = |e: chd::Error| DiscError::Format(format!("{}: {e}", path.display()));

    // the track list lives in the metadata; read it through a second handle (the metadata iterator borrows the file)
    let mut meta_file = File::open(path).map_err(io)?;
    let refs: Vec<_> = {
        let mut c = Chd::open(&mut meta_file, None).map_err(bad)?;
        c.metadata_refs().collect()
    };
    let mut metas = Vec::new();
    for r in refs {
        let tag = r.metatag();
        if tag == KnownMetadata::CdRomTrack2.metatag() || tag == KnownMetadata::CdRomTrack.metatag() {
            let m = r.read(&mut meta_file).map_err(bad)?;
            let text = String::from_utf8_lossy(&m.value).into_owned();
            metas.push(parse_track(&text).ok_or_else(|| DiscError::Format(format!("bad track entry '{text}'")))?);
        }
    }
    if metas.is_empty() {
        return Err(DiscError::Format("this CHD is not a CD image".into()));
    }
    for t in &metas {
        match t.mode.as_str() {
            "AUDIO" | "MODE1_RAW" | "MODE2_RAW" => {}
            other => {
                return Err(DiscError::Unsupported(format!(
                    "CHD track mode {other}; re-create the CHD from the .cue/.bin"
                )))
            }
        }
    }

    let chd = Chd::open(BufReader::new(File::open(path).map_err(io)?), None).map_err(bad)?;
    let hunk_bytes = chd.header().hunk_size() as usize;
    if hunk_bytes % FRAME_BYTES != 0 {
        return Err(DiscError::Format("unexpected CHD hunk size".into()));
    }
    let lay = layout(&metas);
    // like a cue sheet: a track runs up to where the next one starts
    let tracks: Vec<Track> = lay
        .tracks
        .iter()
        .enumerate()
        .map(|(i, &(number, audio, start, _))| {
            let end = lay.tracks.get(i + 1).map_or(lay.total, |n| n.2);
            Track { number, audio, start_lba: start, length: end.saturating_sub(start) }
        })
        .collect();
    let label = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "disc".into());
    let source = ChdSource {
        hunk: chd.get_hunksized_buffer(),
        frames_per_hunk: (hunk_bytes / FRAME_BYTES) as u32,
        chd,
        runs: lay.runs,
        scratch: Vec::new(),
        loaded: None,
    };
    Ok(DiscImage::new(Box::new(source), tracks, lay.total, label))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_chdman_metadata() {
        let t =
            parse_track("TRACK:2 TYPE:AUDIO SUBTYPE:NONE FRAMES:15000 PREGAP:150 PGTYPE:VAUDIO PGSUB:NONE POSTGAP:0\0")
                .unwrap();
        assert_eq!(t, TrackMeta { number: 2, mode: "AUDIO".into(), frames: 15000, pregap: 150, pregap_in_file: true });
        let t =
            parse_track("TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:100 PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0")
                .unwrap();
        assert!(!t.pregap_in_file && t.pregap == 0);
    }

    #[test]
    fn layout_follows_chdman_conventions() {
        let tracks = [
            TrackMeta { number: 1, mode: "MODE2_RAW".into(), frames: 6667, pregap: 0, pregap_in_file: false },
            TrackMeta { number: 2, mode: "AUDIO".into(), frames: 12605, pregap: 833, pregap_in_file: true },
            TrackMeta { number: 3, mode: "AUDIO".into(), frames: 11522, pregap: 153, pregap_in_file: true },
        ];
        let l = layout(&tracks);
        assert_eq!(l.tracks[0], (1, false, 0, 6667));
        // the pregap is part of FRAMES: the track proper starts `pregap` frames into its region
        assert_eq!(l.tracks[1], (2, true, 6667 + 833, 12605 - 833));
        assert_eq!(l.tracks[2], (3, true, 6667 + 12605 + 153, 11522 - 153));
        assert_eq!(l.total, 6667 + 12605 + 11522);
        // file frames: track 1 padded 6667 -> 6668, track 2 (12605 -> 12608) follows
        let t2 = l.runs.iter().find(|r| r.disc_start == 150 + 6667).unwrap();
        assert_eq!(t2.file_frame, Some(6668));
        let t3 = l.runs.iter().find(|r| r.disc_start == 150 + 6667 + 12605).unwrap();
        assert_eq!(t3.file_frame, Some(6668 + 12608));
    }
}
