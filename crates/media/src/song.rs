//! Decoding a song (mp3, m4a/aac, flac, ogg, wav, ...) to the 44.1 kHz stereo 16-bit PCM of an audio CD.

use rubato::{FftFixedIn, Resampler};
use std::fs::File;
use std::path::Path;
use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::{DecoderOptions, CODEC_TYPE_NULL};
use symphonia::core::errors::Error as SymError;
use symphonia::core::formats::FormatOptions;
use symphonia::core::io::MediaSourceStream;
use symphonia::core::meta::MetadataOptions;
use symphonia::core::probe::Hint;

const CD_RATE: usize = 44_100;
/// An audio CD holds 80 minutes.
const MAX_SECONDS: usize = 80 * 60;

#[derive(Debug)]
pub enum SongError {
    Io(std::io::Error),
    Decode(String),
}
impl std::fmt::Display for SongError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SongError::Io(e) => write!(f, "cannot read the song: {e}"),
            SongError::Decode(m) => write!(f, "cannot decode the song: {m}"),
        }
    }
}
impl std::error::Error for SongError {}
impl From<std::io::Error> for SongError {
    fn from(e: std::io::Error) -> Self {
        SongError::Io(e)
    }
}
fn dec<E: std::fmt::Display>(e: E) -> SongError {
    SongError::Decode(e.to_string())
}

pub struct Song {
    pub title: String,
    /// Interleaved stereo samples at 44.1 kHz.
    pub pcm: Vec<i16>,
}

impl Song {
    pub fn seconds(&self) -> f32 {
        self.pcm.len() as f32 / 2.0 / CD_RATE as f32
    }
}

pub fn decode(path: &Path) -> Result<Song, SongError> {
    let file = File::open(path)?;
    let mss = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    if let Some(ext) = path.extension().and_then(|e| e.to_str()) {
        hint.with_extension(ext);
    }
    let probed = symphonia::default::get_probe()
        .format(&hint, mss, &FormatOptions::default(), &MetadataOptions::default())
        .map_err(dec)?;
    let mut format = probed.format;
    let track = format
        .tracks()
        .iter()
        .find(|t| t.codec_params.codec != CODEC_TYPE_NULL)
        .ok_or_else(|| SongError::Decode("no audio track".into()))?;
    let track_id = track.id;
    let mut decoder =
        symphonia::default::get_codecs().make(&track.codec_params, &DecoderOptions::default()).map_err(dec)?;

    let mut rate = track.codec_params.sample_rate.unwrap_or(CD_RATE as u32) as usize;
    let mut left: Vec<f32> = Vec::new();
    let mut right: Vec<f32> = Vec::new();
    let mut scratch: Option<SampleBuffer<f32>> = None;

    loop {
        let packet = match format.next_packet() {
            Ok(p) => p,
            Err(SymError::IoError(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(SymError::ResetRequired) => break,
            Err(e) => return Err(dec(e)),
        };
        if packet.track_id() != track_id {
            continue;
        }
        let decoded = match decoder.decode(&packet) {
            Ok(d) => d,
            Err(SymError::DecodeError(_)) => continue, // skip a corrupt packet
            Err(e) => return Err(dec(e)),
        };
        let spec = *decoded.spec();
        rate = spec.rate as usize;
        let ch = spec.channels.count().max(1);
        if scratch.as_ref().map_or(true, |s| s.capacity() < decoded.capacity() * ch) {
            scratch = Some(SampleBuffer::<f32>::new(decoded.capacity() as u64, spec));
        }
        let sb = scratch.as_mut().unwrap();
        sb.copy_interleaved_ref(decoded);
        for frame in sb.samples().chunks_exact(ch) {
            left.push(frame[0]);
            right.push(if ch >= 2 { frame[1] } else { frame[0] });
        }
        if left.len() > MAX_SECONDS * rate {
            break;
        }
    }
    if left.is_empty() {
        return Err(SongError::Decode("the file contains no audio".into()));
    }

    let (left, right) = if rate == CD_RATE { (left, right) } else { resample(&left, &right, rate)? };
    let mut pcm = Vec::with_capacity(left.len().min(MAX_SECONDS * CD_RATE) * 2);
    for (l, r) in left.iter().zip(&right).take(MAX_SECONDS * CD_RATE) {
        pcm.push((l * 32767.0).round().clamp(-32768.0, 32767.0) as i16);
        pcm.push((r * 32767.0).round().clamp(-32768.0, 32767.0) as i16);
    }
    let title = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_else(|| "song".into());
    Ok(Song { title, pcm })
}

fn resample(left: &[f32], right: &[f32], rate: usize) -> Result<(Vec<f32>, Vec<f32>), SongError> {
    let mut rs = FftFixedIn::<f32>::new(rate, CD_RATE, 4096, 2, 2).map_err(dec)?;
    let delay = rs.output_delay();
    let expect = (left.len() as u64 * CD_RATE as u64).div_ceil(rate as u64) as usize;
    let (mut ol, mut orr) = (Vec::with_capacity(expect + delay + 4096), Vec::with_capacity(expect + delay + 4096));
    let mut pos = 0;
    loop {
        let need = rs.input_frames_next();
        if left.len() - pos < need {
            break;
        }
        let out = rs.process(&[&left[pos..pos + need], &right[pos..pos + need]], None).map_err(dec)?;
        ol.extend_from_slice(&out[0]);
        orr.extend_from_slice(&out[1]);
        pos += need;
    }
    let out = rs.process_partial(Some(&[&left[pos..], &right[pos..]]), None).map_err(dec)?;
    ol.extend_from_slice(&out[0]);
    orr.extend_from_slice(&out[1]);
    let out = rs.process_partial::<&[f32]>(None, None).map_err(dec)?;
    ol.extend_from_slice(&out[0]);
    orr.extend_from_slice(&out[1]);
    let skip = delay.min(ol.len());
    ol.drain(..skip);
    orr.drain(..skip);
    ol.truncate(expect);
    orr.truncate(expect);
    Ok((ol, orr))
}
