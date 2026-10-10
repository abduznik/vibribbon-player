fn main() {
    println!("cargo:rerun-if-changed=c/audioout.c");
    println!("cargo:rerun-if-changed=c/miniaudio.h");
    let mut b = cc::Build::new();
    b.file("c/audioout.c").include("c").opt_level(2).warnings(false).flag_if_supported("-Wno-unused-function");
    b.compile("audioout_c");
    match std::env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("linux") | Ok("freebsd") | Ok("netbsd") | Ok("openbsd") => {
            println!("cargo:rustc-link-lib=dl");
            println!("cargo:rustc-link-lib=pthread");
            println!("cargo:rustc-link-lib=m");
        }
        Ok("macos") => {
            for f in ["CoreFoundation", "CoreAudio", "AudioToolbox"] {
                println!("cargo:rustc-link-lib=framework={f}");
            }
        }
        _ => {}
    }
}
