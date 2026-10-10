#![cfg_attr(all(not(debug_assertions), windows), windows_subsystem = "windows")]

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("--headless") {
        std::process::exit(vibribbon_player::headless::run(&args[1..]));
    }
    let mut viewport = eframe::egui::ViewportBuilder::default()
        .with_title("Vib-Ribbon Player")
        .with_app_id("vibribbon-player")
        .with_inner_size([960.0, 720.0])
        .with_min_inner_size([480.0, 360.0])
        .with_drag_and_drop(true);
    if let Some(icon) = vibribbon_player::branding::window_icon() {
        viewport = viewport.with_icon(std::sync::Arc::new(icon));
    }
    let options = eframe::NativeOptions { viewport, ..Default::default() };
    let result = eframe::run_native(
        "Vib-Ribbon Player",
        options,
        Box::new(move |cc| Ok(Box::new(vibribbon_player::app::App::new(cc).with_args(&cc.egui_ctx, &args)))),
    );
    if let Err(e) = result {
        eprintln!("cannot open a window: {e}");
        std::process::exit(1);
    }
}
