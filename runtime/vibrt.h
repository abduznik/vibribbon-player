/* vibrt: the vibrecomp core as a library.
 *
 * One machine per process (the core keeps its state in globals). Typical use:
 *   - build a VibrtDisc for the game from a sector-reading callback (any image format) and one for the song,
 *   - vibrt_set_callbacks(), then call vibrt_run() on a dedicated emulation thread;
 *   - from the `frame` callback (emulation thread, once per vblank): set the controller, swap discs, render;
 *   - pull audio with vibrt_audio_pull() from the audio device callback;
 *   - vibrt_request_stop() from any thread ends vibrt_run().
 * Discs passed to the core stay owned by the caller and must outlive the run.
 */
#ifndef VIBRT_H
#define VIBRT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Disc VibrtDisc;

/* Reads one raw 2352-byte sector (absolute LBA, 0 = start of the program area). Returns 0 on success. */
typedef int (*VibrtReadSectorFn)(void *user, uint32_t lba, uint8_t *out2352);

typedef struct {
    int32_t number;        /* 1-based */
    int32_t audio;         /* nonzero for CD-DA */
    uint32_t start_lba;    /* absolute LBA of INDEX 01 */
    uint32_t length;       /* sectors */
} VibrtTrack;

/* `destroy_user` (optional) is called with `user` when the disc is freed. */
VibrtDisc *vibrt_disc_new(const VibrtTrack *tracks, int ntracks, uint32_t total_sectors,
                          VibrtReadSectorFn read, void *user, void (*destroy_user)(void *user), const char *label);
/* A one-track audio CD made of 44.1 kHz stereo signed 16-bit PCM (copied). */
VibrtDisc *vibrt_disc_from_pcm(const int16_t *interleaved, size_t frames, const char *label);
void vibrt_disc_free(VibrtDisc *d);
/* Executable named by SYSTEM.CNF (e.g. "SCES_028.73"); returns 1 on success. */
int vibrt_disc_boot_name(VibrtDisc *d, char *out, size_t out_len);
/* 1 if the ISO9660 file system of the disc contains `path` (e.g. "MAIN_G.EXE"). */
int vibrt_disc_has_file(VibrtDisc *d, const char *path);

typedef struct {
    /* Emulation thread, once per vblank, before the game reads the pad. */
    void (*frame)(void *user, uint32_t frame_no);
    /* Emulation thread, whenever the game sends a command to the CD drive (command byte, e.g. 0x03 = Play). */
    void (*cd_command)(void *user, uint8_t cmd);
} VibrtCallbacks;
void vibrt_set_callbacks(const VibrtCallbacks *cb, void *user);

/* Runs the game until it exits or vibrt_request_stop(). `boot_exe` NULL = the executable named by SYSTEM.CNF.
 * Returns the number of frames run. */
int vibrt_run(VibrtDisc *game, int pal, const char *boot_exe);
void vibrt_request_stop(void);
int vibrt_stop_requested(void);

/* Controller 1, digital pad. Bit set = pressed: 0x0001 select, 0x0008 start, 0x0010 up, 0x0020 right,
 * 0x0040 down, 0x0080 left, 0x0100 L2, 0x0200 R2, 0x0400 L1, 0x0800 R1, 0x1000 triangle, 0x2000 circle,
 * 0x4000 cross, 0x8000 square. */
void vibrt_set_pad(uint16_t pressed);

/* Opens the lid, replaces the disc and closes the lid again (open_ms of emulated time). */
void vibrt_swap_disc(VibrtDisc *d, uint32_t open_ms);

/* Fast-forward: no real-time pacing (the caller should also mute audio). CD turbo: data reads/seeks 10x faster. */
void vibrt_set_fast_forward(int on);
void vibrt_set_cd_turbo(int on);
void vibrt_set_verbose(int level);
/* Real-time pacing against the audio ring (default on). Off = run as fast as possible (tests, headless). */
void vibrt_set_pacing(int on);

/* Audio produced by the SPU: 44.1 kHz stereo s16, interleaved. Safe to call from the audio thread. */
size_t vibrt_audio_fill(void);
size_t vibrt_audio_pull(int16_t *dst, size_t frames);
void vibrt_audio_clear(void);

/* Current picture as RGBA bytes (R, G, B, A in memory order) into `rgba` (at least 1024x512 pixels at the given
 * stride). Returns 1 and sets *w, *h when the display is enabled, 0 (all black, size still set) otherwise.
 * Call from the frame callback. */
int vibrt_render_frame(uint32_t *rgba, size_t stride_pixels, int *w, int *h);

/* Platform self test (coroutine switching). Returns 0 on success. */
int vibrt_selftest(void);

#ifdef __cplusplus
}
#endif
#endif
