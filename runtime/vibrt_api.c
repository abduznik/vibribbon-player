/* Implementation of vibrt.h on top of the core. Not part of the C test harnesses (they supply their own
 * frontend_present/frontend_pace). */
#include "coro.h"
#include "psx.h"
#include "vibrt.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

static VibrtCallbacks g_cb;
static void *g_cb_user;
static uint32_t g_frame_no;
static int g_pacing = 1;

static void sleep_1ms(void) {
#ifdef _WIN32
    Sleep(1);
#else
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
#endif
}

/* ------------------------------------------------------------ frontend hooks required by the core */
void frontend_present(void) {
    g_frame_no++;
    if (g_cb.frame) g_cb.frame(g_cb_user, g_frame_no);
}

/* keep emulated time locked to the audio device: never run more than ~60 ms ahead of what has been played */
void frontend_pace(void) {
    if (g_fast_forward || !g_pacing) return;
    while (!g_quit && spu_ring_fill() > 44100 * 60 / 1000) sleep_1ms();
}

static void cd_hook(u8 cmd) {
    if (g_cb.cd_command) g_cb.cd_command(g_cb_user, cmd);
}

/* ------------------------------------------------------------ discs */
typedef struct {
    VibrtReadSectorFn read;
    void *user;
    void (*destroy_user)(void *user);
} CbDisc;

static int cb_read_sector(Disc *d, u32 lba, u8 *out) {
    CbDisc *c = d->priv;
    if (c->read(c->user, lba, out) == 0) return 0;
    memset(out, 0, 2352);
    return -1;
}
static void cb_destroy(Disc *d) {
    CbDisc *c = d->priv;
    if (c->destroy_user) c->destroy_user(c->user);
    free(c);                           /* disc_free releases the Disc itself */
}

VibrtDisc *vibrt_disc_new(const VibrtTrack *tracks, int ntracks, uint32_t total_sectors,
                          VibrtReadSectorFn read, void *user, void (*destroy_user)(void *user), const char *label) {
    if (!read || ntracks < 1 || ntracks > MAX_TRACKS) return NULL;
    Disc *d = calloc(1, sizeof *d);
    CbDisc *c = calloc(1, sizeof *c);
    c->read = read; c->user = user; c->destroy_user = destroy_user;
    d->priv = c;
    d->read_sector = cb_read_sector;
    d->destroy = cb_destroy;
    d->ntracks = ntracks;
    for (int i = 0; i < ntracks; i++) {
        d->tracks[i + 1].number = tracks[i].number ? tracks[i].number : i + 1;
        d->tracks[i + 1].audio = tracks[i].audio != 0;
        d->tracks[i + 1].start_lba = tracks[i].start_lba;
        d->tracks[i + 1].length = tracks[i].length;
    }
    d->total_sectors = total_sectors;
    d->is_psx_data_disc = !d->tracks[1].audio;
    snprintf(d->label, sizeof d->label, "%s", label ? label : "disc");
    return d;
}

VibrtDisc *vibrt_disc_from_pcm(const int16_t *interleaved, size_t frames, const char *label) {
    return disc_open_audio_pcm(interleaved, frames, label);
}
void vibrt_disc_free(VibrtDisc *d) { disc_free(d); }
int vibrt_disc_boot_name(VibrtDisc *d, char *out, size_t n) { return disc_boot_name(d, out, n); }
int vibrt_disc_has_file(VibrtDisc *d, const char *path) { IsoFile f; return iso_find(d, path, &f); }

/* ------------------------------------------------------------ run control */
void vibrt_set_callbacks(const VibrtCallbacks *cb, void *user) {
    if (cb) g_cb = *cb; else memset(&g_cb, 0, sizeof g_cb);
    g_cb_user = user;
    g_cd_command_hook = (cb && cb->cd_command) ? cd_hook : NULL;
}

int vibrt_run(VibrtDisc *game, int pal, const char *boot_exe) {
    g_frame_no = 0;
    g_force_interp = true;              /* the library carries no recompiled game code */
    g_pad_override_active = false;
    runtime_reset(game, pal != 0);
    runtime_run(boot_exe);
    return (int)g_frame_no;
}
void vibrt_request_stop(void) { g_quit = true; }
int vibrt_stop_requested(void) { return g_quit; }

void vibrt_set_pad(uint16_t pressed) { g_pad_buttons = (u16)~pressed; }
void vibrt_swap_disc(VibrtDisc *d, uint32_t open_ms) { cd_begin_swap(d, (u64)open_ms * (CPU_HZ / 1000)); }
void vibrt_set_fast_forward(int on) { g_fast_forward = on != 0; }
void vibrt_set_cd_turbo(int on) { g_cd_turbo = on != 0; }
void vibrt_set_verbose(int level) { g_verbose = level; }
void vibrt_set_pacing(int on) { g_pacing = on != 0; }

size_t vibrt_audio_fill(void) { return spu_ring_fill(); }
size_t vibrt_audio_pull(int16_t *dst, size_t frames) { return spu_ring_pull(dst, frames); }
void vibrt_audio_clear(void) { spu_ring_clear(); }

/* ------------------------------------------------------------ video */
int vibrt_render_frame(uint32_t *rgba, size_t stride, int *w, int *h) {
    DisplayInfo d;
    gpu_get_display(&d);
    int ww = d.w, hh = d.h;
    if (ww > 1024) ww = 1024;
    if (hh > 512) hh = 512;
    if (ww < 1) ww = 1;
    if (hh < 1) hh = 1;
    *w = ww; *h = hh;
    if (!d.enabled) {
        for (int y = 0; y < hh; y++) for (int x = 0; x < ww; x++) rgba[y * stride + x] = 0xff000000u;
        return 0;
    }
    if (!d.depth24) {
        for (int y = 0; y < hh; y++) {
            const u16 *src = &vram[((d.y + y) & 511) * 1024];
            u32 *row = rgba + (size_t)y * stride;
            for (int x = 0; x < ww; x++) {
                u16 p = src[(d.x + x) & 1023];
                u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                r |= r >> 5; g |= g >> 5; b |= b >> 5;
                row[x] = 0xff000000u | (b << 16) | (g << 8) | r;     /* bytes in memory: R G B A */
            }
        }
    } else {
        for (int y = 0; y < hh; y++) {
            const u8 *src = (const u8 *)&vram[((d.y + y) & 511) * 1024];
            u32 *row = rgba + (size_t)y * stride;
            for (int x = 0; x < ww; x++) {
                const u8 *p = src + ((d.x * 2 + x * 3) & 2047);
                row[x] = 0xff000000u | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------ self test */
static Coro st_main, st_co;
static int st_counter, st_done, st_longjmp_ok;
static void st_body(void) {
    for (int i = 0; i < 1000; i++) { st_counter += 2; coro_switch(&st_co, &st_main); }
    /* the kernel unwinds interrupt handlers with longjmp after having switched coroutines in between */
    jmp_buf jb;
    if (setjmp(jb) == 0) {
        coro_switch(&st_co, &st_main);
        longjmp(jb, 1);
    }
    st_longjmp_ok = 1;
    st_done = 1;
    coro_jump(&st_main);
}
/* Returns 0 when coroutine switching (the HLE thread scheduler's foundation) works on this platform. */
int vibrt_selftest(void) {
    static char stack[256 * 1024];
    coro_thread_init();
    st_counter = st_done = st_longjmp_ok = 0;
    coro_create(&st_co, stack, sizeof stack, st_body);
    for (int i = 0; i < 1000; i++) { st_counter += 1; coro_switch(&st_main, &st_co); }
    coro_switch(&st_main, &st_co);          /* runs the body up to its setjmp/switch */
    coro_switch(&st_main, &st_co);          /* resumes it: longjmp, then finish */
    int ok = st_done && st_longjmp_ok && st_counter == 3000;
    coro_destroy(&st_co);
    return ok ? 0 : 1;
}
