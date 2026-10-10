// Embeds the application icon into the Windows executable (a no-op elsewhere; never fails the build).
fn main() {
    println!("cargo:rerun-if-changed=assets/icon.ico");
    #[cfg(windows)]
    {
        if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
            let mut res = winresource::WindowsResource::new();
            res.set_icon("assets/icon.ico");
            res.set("ProductName", "Vib-Ribbon Player");
            res.set("FileDescription", "Play Vib-Ribbon with any song");
            if let Err(e) = res.compile() {
                println!("cargo:warning=could not embed the Windows icon: {e}");
            }
        }
    }
}
