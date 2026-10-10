use std::path::PathBuf;

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../runtime");
    // everything in runtime/ except the C test frontends, the dev-only decoders and the autoplay script
    let sources = [
        "mem.c",
        "cpu.c",
        "gte.c",
        "gpu.c",
        "irq.c",
        "timers.c",
        "dma.c",
        "cdrom.c",
        "disc.c",
        "spu.c",
        "sio.c",
        "kernel.c",
        "guest.c",
        "biosnames.c",
        "ctrl.c",
        "runtime.c",
        "vibrt_api.c",
    ];
    let mut b = cc::Build::new();
    b.include(&root)
        .std("gnu11")
        .define("VIBRT_NO_OVERLAYS", None)
        .opt_level(2)
        .debug(false)
        .warnings(false)
        .flag_if_supported("-fno-strict-aliasing")
        .flag_if_supported("-Wno-unused-function");
    for s in sources {
        b.file(root.join(s));
        println!("cargo:rerun-if-changed={}", root.join(s).display());
    }
    println!("cargo:rerun-if-changed={}", root.join("psx.h").display());
    println!("cargo:rerun-if-changed={}", root.join("vibrt.h").display());
    println!("cargo:rerun-if-changed={}", root.join("ops.h").display());
    b.compile("vibrt_core");
    if std::env::var("CARGO_CFG_TARGET_OS").map(|o| o != "windows").unwrap_or(true) {
        println!("cargo:rustc-link-lib=m");
    }
}
