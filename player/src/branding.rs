//! The logo, window icon and the colour theme that goes with them.

use eframe::egui::{self, Color32};

/// Logo for the launcher (512x512) and the window icon (128x128), made from `assets/logo.png` by
/// `tools/make_icons.py`.
pub const LOGO_PNG: &[u8] = include_bytes!("../assets/logo-512.png");
pub const ICON_PNG: &[u8] = include_bytes!("../assets/icon-128.png");

/// The logo's own background colour; the launcher uses it so the picture blends in.
pub const BACKGROUND: Color32 = Color32::from_rgb(7, 5, 15);

pub struct Image {
    pub width: usize,
    pub height: usize,
    pub rgba: Vec<u8>,
}

/// Decodes a PNG (8 or 16 bit, with or without alpha) to RGBA8.
pub fn decode_png(bytes: &[u8]) -> Option<Image> {
    let mut decoder = png::Decoder::new(std::io::Cursor::new(bytes));
    decoder.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut reader = decoder.read_info().ok()?;
    let mut buf = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut buf).ok()?;
    let data = &buf[..info.buffer_size()];
    let (width, height) = (info.width as usize, info.height as usize);
    let rgba = match info.color_type {
        png::ColorType::Rgba => data.to_vec(),
        png::ColorType::Rgb => data.chunks_exact(3).flat_map(|p| [p[0], p[1], p[2], 255]).collect(),
        png::ColorType::GrayscaleAlpha => data.chunks_exact(2).flat_map(|p| [p[0], p[0], p[0], p[1]]).collect(),
        png::ColorType::Grayscale => data.iter().flat_map(|&g| [g, g, g, 255]).collect(),
        png::ColorType::Indexed => return None,
    };
    Some(Image { width, height, rgba })
}

pub fn window_icon() -> Option<egui::IconData> {
    decode_png(ICON_PNG).map(|i| egui::IconData { rgba: i.rgba, width: i.width as u32, height: i.height as u32 })
}

/// Dark navy with magenta accents, matching the logo.
pub fn apply_theme(ctx: &egui::Context) {
    let mut v = egui::Visuals::dark();
    v.panel_fill = BACKGROUND;
    v.window_fill = Color32::from_rgb(16, 12, 30);
    v.extreme_bg_color = Color32::from_rgb(3, 2, 8);
    v.faint_bg_color = Color32::from_rgb(18, 14, 32);
    v.widgets.noninteractive.bg_stroke = egui::Stroke::new(1.0_f32, Color32::from_rgb(58, 42, 84));
    v.widgets.noninteractive.fg_stroke = egui::Stroke::new(1.0_f32, Color32::from_rgb(210, 205, 225));
    v.widgets.inactive.weak_bg_fill = Color32::from_rgb(30, 22, 50);
    v.widgets.inactive.bg_fill = Color32::from_rgb(30, 22, 50);
    v.widgets.hovered.weak_bg_fill = Color32::from_rgb(78, 34, 92);
    v.widgets.hovered.bg_fill = Color32::from_rgb(78, 34, 92);
    v.widgets.active.weak_bg_fill = Color32::from_rgb(140, 40, 128);
    v.widgets.active.bg_fill = Color32::from_rgb(140, 40, 128);
    v.selection.bg_fill = Color32::from_rgb(150, 44, 134);
    v.selection.stroke = egui::Stroke::new(1.0_f32, Color32::WHITE);
    ctx.set_visuals(v);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn embedded_images_decode() {
        let logo = decode_png(LOGO_PNG).expect("logo");
        assert_eq!((logo.width, logo.height), (512, 512));
        assert_eq!(logo.rgba.len(), 512 * 512 * 4);
        let icon = window_icon().expect("icon");
        assert_eq!((icon.width, icon.height), (128, 128));
        // the logo's corner is the background colour the theme uses
        assert_eq!(&logo.rgba[..3], &[7, 5, 15]);
    }
}
