/* Autoplay: drives the game's own menus so that handing the runtime a song starts the game with it.
 *
 * Boot MAIN_G.EXE directly (skips loader and title), then, using synthetic controller input:
 *   main menu -> "Play with my own choice of CD" -> "Single track" -> (game asks for a music CD)
 *   -> virtual disc swap with the song on it -> track picker -> track 1.
 * Everything runs fast-forwarded (and muted) until the game starts the song, then real time begins.
 * Timing is expressed in frames since boot; the emulation is deterministic so this is repeatable. */
#include "psx.h"

static struct {
    bool armed;
    Disc *song;
    int frame;
    int stage;
    int press_until;
    u16 press_mask;
    int swap_done_frame;
    int play_seen_frame;
    int retry_at;
    bool play_seen;
} ap;

enum { BTN_DOWN = 0x40, BTN_CROSS = 0x4000 };

/* stage schedule, in frames since boot */
#define F_DOWN       220
#define F_OWN_CD     280
#define F_SINGLE     400
#define F_SWAP       500
#define SWAP_OPEN_CYCLES (CPU_HZ / 2)       /* lid open for ~0.5 s of emulated time */
#define PRESS_FRAMES 2                       /* a short tap: longer holds auto-repeat in the game menus */
#define TRACK_PICK_DELAY 120                 /* frames after the game starts probing the new disc */

static void press(u16 mask, int frames) {
    LOG("[autoplay] frame %d: press %04x for %d frames (stage %d)\n", ap.frame, mask, frames, ap.stage);
    ap.press_mask = mask;
    ap.press_until = ap.frame + frames;
}

static void on_cd_command(u8 cmd) {
    /* the game sends Play(03) while probing the freshly inserted disc, and again (preceded by Mute) when a
     * track is chosen. The first one tells us the track picker is about to be shown. */
    if (!ap.armed) return;
    if (cmd == 0x03 && ap.swap_done_frame && !ap.play_seen) { ap.play_seen = true; ap.play_seen_frame = ap.frame; LOG("[autoplay] frame %d: disc probe Play seen\n", ap.frame); }
    if (cmd == 0x0b && ap.stage == 5) {
        /* Mute + Play of the chosen track: gameplay (real time) starts now */
        ap.stage = 7;
        LOG("[autoplay] frame %d: song started\n", ap.frame);
    }
}

void autoplay_init(Disc *song) {
    memset(&ap, 0, sizeof ap);
    ap.armed = song != NULL;
    ap.song = song;
    g_fast_forward = ap.armed;
    g_cd_turbo = ap.armed;
    g_pad_override_active = ap.armed;
    g_pad_override = 0xffff;
    g_cd_command_hook = ap.armed ? on_cd_command : NULL;
}

bool autoplay_active(void) { return ap.armed && ap.stage < 8; }

/* called once per vblank, before the game reads the pad */
void autoplay_on_frame(void) {
    if (!ap.armed) return;
    ap.frame++;
    if (ap.press_mask && ap.frame >= ap.press_until) ap.press_mask = 0;
    g_pad_override = (u16)~ap.press_mask;

    switch (ap.stage) {
    case 0: if (ap.frame == F_DOWN) { press(BTN_DOWN, PRESS_FRAMES); ap.stage = 1; } break;
    case 1: if (ap.frame == F_OWN_CD) { press(BTN_CROSS, PRESS_FRAMES); ap.stage = 2; } break;
    case 2: if (ap.frame == F_SINGLE) { press(BTN_CROSS, PRESS_FRAMES); ap.stage = 3; } break;
    case 3:
        if (ap.frame == F_SWAP) {
            cd_begin_swap(ap.song, SWAP_OPEN_CYCLES);
            ap.swap_done_frame = ap.frame;
            ap.stage = 4;
        }
        break;
    case 4:
        if (ap.play_seen && ap.frame >= ap.play_seen_frame + TRACK_PICK_DELAY) {
            press(BTN_CROSS, PRESS_FRAMES);
            ap.retry_at = ap.frame + 40;
            ap.stage = 5;
        }
        break;
    case 5:
        /* the picker may not accept input immediately: keep pressing until Mute+Play shows the song started */
        if (ap.frame >= ap.retry_at && ap.frame < ap.play_seen_frame + 3000) { press(BTN_CROSS, PRESS_FRAMES); ap.retry_at = ap.frame + 40; }
        break;
    case 7:
        /* song started: leave fast-forward, hand control to the player */
        g_fast_forward = false;
        g_cd_turbo = false;
        g_pad_override_active = false;
        ap.stage = 8;
        break;
    default: break;
    }
}
