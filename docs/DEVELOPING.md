# Developing

## Layout

| Path | What |
|---|---|
| `runtime/` | The machine in C (`vibrt.h` is the public API, `vibrt_api.c` implements it). `main_headless.c` is a C test harness. |
| `crates/vibrt` | Rust FFI + safe API; its `build.rs` compiles `runtime/`. |
| `crates/media` | CUE/BIN and CHD readers, song decoding. `cargo run --example cmp_discs -- a.cue b.chd` compares two images sector by sector. |
| `crates/macros` | Macro engine and profiles. A profile (`profiles/*.toml`) describes one disc and its macros as lists of steps. |
| `crates/audioout` | Audio device. |
| `player/` | The app (`vibribbon-player`), plus a `--headless` mode. |
| `assets/` | The logo (`logo.png`); `tools/make_icons.py` makes the launcher logo, window icon and Windows `.ico` in `player/assets/`. |
| `tools/` | Python tools: MIPS disassembler/analysis, the static recompiler experiment, lock-step verification. |

## Headless runs (no window)

```sh
cargo build --release -p vibribbon-player
target/release/vibribbon-player --headless --disc game.cue --song song.flac --macro song --frames 3000 --dump out 100
```

Runs a macro as fast as possible, prints when it finishes (`macro done at frame 795`) and optionally dumps
frames as PPM. This is the regression test for the whole product path; the song macro for the European disc
must finish at frame 795.

## Macros

Steps run in order; the engine is driven once per video frame (50 Hz PAL):

```toml
steps = [
    { fast_forward = true, turbo = true },        # skip through menus unthrottled and muted
    { wait_until = 220 },                         # absolute frame number since start
    { press = "down" },                           # a tap (1 frame); `hold = 3` for longer
    { wait = 60 },                                # frames, relative to the start of the step
    { swap_song = true },                         # virtual disc swap
    { wait_cd = "play", timeout = 3000 },         # wait for a CD drive command from the game
    { press = "cross", every = 40, until_cd = "mute", timeout = 3000 },   # retry until the game reaches a state
]
```

The emulation is deterministic, so frame numbers are stable. Timing is not forgiving in the game menus (long
presses auto-repeat; a menu ignores presses while it animates), so scripts are tuned per disc and verified with
`--headless`.

## The static recompiler (experiment, not shipped)

`tools/recomp.py` translates the game's MIPS code to C and was verified bit-exact against the interpreter
(`tools/verify.sh`). The generated code is derived from the game and **must never be committed or released**
(`gen/` is in `.gitignore`); the player uses the interpreter, which is already several times faster than real
time. See `make recomp`.

## Releases

* `ci.yml` runs the tests on Linux, macOS (Apple silicon) and Windows (MinGW-w64 / GNU target) for every pull
  request and branch push, plus a cross-build for Intel Macs. The tests that need a disc image or a song skip
  themselves when the files are absent.
* `release.yml` runs `ci.yml` and then builds the player for Linux x86_64, Windows x86_64, macOS arm64 and
  macOS x86_64 (a `.app` bundle with an ad-hoc signature), writes `SHA256SUMS`, and publishes:
  * a push to `main` replaces the `nightly` pre-release;
  * a pushed tag `vX.Y.Z` (which must equal the version in the workspace `Cargo.toml`) creates a normal release.

  To release: bump `version` in the root `Cargo.toml`, commit, `git tag vX.Y.Z && git push --tags`.
* Windows is built with the GNU target because the C core needs gcc/clang; an MSVC build has not been tried.
* The first runs of these workflows may need small fixes: they could not be run while writing them. The Rust
  code was cross-checked for Windows (GNU) and, apart from the GUI crate, for macOS.
