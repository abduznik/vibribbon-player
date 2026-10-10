/* Headless harness: boots the game disc, runs N frames, dumps display frames as PPM. For bring-up/testing. */
#include "psx.h"
#include <stdlib.h>
#include <time.h>

extern u32 *g_pc_hist;
static const char *ram_dump_path;
static const char *pc_hist_out;
static u32 fuzz_seed, fuzz_from = 900;
static u32 fuzz_state;
static int frame_no;
static int max_frames = 300;
static const char *dump_dir;
static int dump_every;
static const char *swap_file;
static int swap_frame = -1;
static int hist_from = -1;
static int spu_dump_frame = -1;
extern int g_spu_dma_log, g_spu_dma_rlog;
static FILE *wav_f;
static u64 wav_frames;
extern void spu_dump_state(void);
static const char *boot_exe;
static const char *autoplay_file;
static const char *entries_file;
extern void cpu_trace_entries_start(void); extern void cpu_trace_entries_dump(const char *);
extern int g_gpu_log_frame, g_gpu_frame_ctr, g_gte_debug;

typedef struct { int frame, hold; u16 mask; } Press;
static Press presses[256];
static int npress;

static u16 button_mask(const char *n) {
    static const struct { const char *n; u16 m; } t[] = {
        {"select", 0x1}, {"start", 0x8}, {"up", 0x10}, {"right", 0x20}, {"down", 0x40}, {"left", 0x80},
        {"l2", 0x100}, {"r2", 0x200}, {"l1", 0x400}, {"r1", 0x800}, {"triangle", 0x1000}, {"circle", 0x2000},
        {"cross", 0x4000}, {"square", 0x8000}};
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(t[i].n, n)) return t[i].m;
    return 0;
}
/* spec: name@frame[+hold] */
static void add_press(const char *spec) {
    char name[32]; int fr = 0, hold = 4;
    if (sscanf(spec, "%31[^@]@%d+%d", name, &fr, &hold) < 2) return;
    if (npress < 256) presses[npress++] = (Press){fr, hold, button_mask(name)};
}

static void dump_frame(const char *path) {
    DisplayInfo d;
    gpu_get_display(&d);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", d.w, d.h);
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) {
            u8 rgb[3] = {0, 0, 0};
            if (d.enabled) {
                if (!d.depth24) {
                    u16 p = vram[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)];
                    rgb[0] = (u8)((p & 31) << 3); rgb[1] = (u8)(((p >> 5) & 31) << 3); rgb[2] = (u8)(((p >> 10) & 31) << 3);
                } else {
                    int byte_x = (d.x * 2 + x * 3);
                    for (int c = 0; c < 3; c++) {
                        int bx = byte_x + c;
                        u16 p = vram[((d.y + y) & 511) * 1024 + ((bx >> 1) & 1023)];
                        rgb[c] = (u8)(bx & 1 ? p >> 8 : p & 255);
                    }
                }
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void wav_flush(void) {
    if (!wav_f) return;
    s16 buf[4096 * 2];
    size_t got;
    while ((got = spu_ring_pull(buf, 4096)) > 0) { fwrite(buf, 4, got, wav_f); wav_frames += got; }
}
void frontend_present(void) {
    wav_flush();
    frame_no++;
    autoplay_on_frame();
    g_gpu_frame_ctr = frame_no + 1;
    u16 b = 0xffff;
    for (int i = 0; i < npress; i++)
        if (frame_no >= presses[i].frame && frame_no < presses[i].frame + presses[i].hold) b &= ~presses[i].mask;
    if (fuzz_seed && frame_no >= fuzz_from) {
        /* deterministic pseudo-random button mashing (identical for every backend) */
        if (frame_no % 3 == 0) {
            fuzz_state ^= fuzz_state << 13; fuzz_state ^= fuzz_state >> 17; fuzz_state ^= fuzz_state << 5;
        }
        u16 m = (u16)(fuzz_state & fuzz_state >> 7 & 0xf0ff);       /* sparse: ~1/4 of the real buttons */
        b &= (u16)~m;
    }
    g_pad_buttons = b;
    if (dump_dir && dump_every && frame_no % dump_every == 0) {
        char p[512];
        snprintf(p, sizeof p, "%s/frame%05d.ppm", dump_dir, frame_no);
        dump_frame(p);
    }
    if (spu_dump_frame > 0 && (frame_no == spu_dump_frame || (frame_no > spu_dump_frame && frame_no % 100 == 0 && frame_no <= spu_dump_frame + 500))) spu_dump_state();
    if (hist_from > 0 && frame_no == hist_from) g_pc_hist = calloc(RAM_SIZE / 4, 4);
    if (swap_file && frame_no == swap_frame) {
        Disc *d = disc_open_audio_file(swap_file);
        if (!d) fprintf(stderr, "cannot decode %s\n", swap_file);
        else { fprintf(stderr, "[swap] inserting %s (%u sectors) at frame %d\n", swap_file, d->total_sectors, frame_no); cd_begin_swap(d, CPU_HZ * 2); }
    }
    if (frame_no >= max_frames) g_quit = true;
}
void frontend_pace(void) {}

extern bool g_trace_io;
extern u32 g_wwatch_lo, g_wwatch_hi;
extern u32 *g_pc_hist;
extern u32 g_break_pc[4]; extern int g_nbreak; extern u32 g_watch_lo, g_watch_hi;
static void print_hist(void) {
    if (!g_pc_hist) return;
    for (int k = 0; k < 25; k++) {
        u32 best = 0, bi = 0;
        for (u32 i = 0; i < RAM_SIZE / 4; i++) if (g_pc_hist[i] > best) { best = g_pc_hist[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "  pc %08x: %u\n", 0x80000000u + bi * 4, best);
        g_pc_hist[bi] = 0;
    }
}

int main(int argc, char **argv) {
    const char *cue = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) cue = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) { dump_dir = argv[++i]; dump_every = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--interp")) g_force_interp = true;
        else if (!strcmp(argv[i], "--sync")) g_sync_mode = true;
        else if (!strcmp(argv[i], "--keyon-log")) { extern int g_spu_keyon_log; g_spu_keyon_log = 1; }
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) { fuzz_seed = (u32)strtoul(argv[++i], NULL, 0); fuzz_state = fuzz_seed * 2654435761u | 1; }
        else if (!strcmp(argv[i], "--fuzz-from") && i + 1 < argc) fuzz_from = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pc-hist-out") && i + 1 < argc) { pc_hist_out = argv[++i]; g_pc_hist = calloc(RAM_SIZE / 4, 4); }
        else if (!strcmp(argv[i], "--learn-writes") && i + 1 < argc) learn_writes_start(argv[++i]);
        else if (!strcmp(argv[i], "--wwatch") && i + 2 < argc) { g_wwatch_lo = (u32)strtoul(argv[++i], NULL, 16); g_wwatch_hi = (u32)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--ram-dump") && i + 1 < argc) ram_dump_path = argv[++i];
        else if (!strcmp(argv[i], "--vtrace") && i + 2 < argc) { vtrace_open(argv[i + 1], strtoull(argv[i + 2], NULL, 0)); i += 2; }
        else if (!strcmp(argv[i], "--swap") && i + 2 < argc) { swap_frame = atoi(argv[++i]); swap_file = argv[++i]; }
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) add_press(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-vv")) g_verbose = 2;
        else if (!strcmp(argv[i], "-vvv")) g_verbose = 3;
        else if (!strcmp(argv[i], "--trace-io")) g_trace_io = true;
        else if (!strcmp(argv[i], "--trace-exec")) g_trace_exec = true;
        else if (!strcmp(argv[i], "--break") && i + 1 < argc && g_nbreak < 4) g_break_pc[g_nbreak++] = (u32)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--pc-hist-from") && i + 1 < argc) hist_from = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-log") && i + 1 < argc) g_gpu_log_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gte-debug")) g_gte_debug = 1;
        else if (!strcmp(argv[i], "--gte-debug2")) g_gte_debug = 2;
        else if (!strcmp(argv[i], "--boot") && i + 1 < argc) boot_exe = argv[++i];
        else if (!strcmp(argv[i], "--turbo")) g_cd_turbo = true;
        else if (!strcmp(argv[i], "--autoplay") && i + 1 < argc) autoplay_file = argv[++i];
        else if (!strcmp(argv[i], "--spu-dump") && i + 1 < argc) spu_dump_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_f = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "--spu-dma-log") && i + 1 < argc) g_spu_dma_log = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 2 < argc) { g_watch_lo = (u32)strtoul(argv[++i], NULL, 16); g_watch_hi = (u32)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--spu-dma-rlog") && i + 1 < argc) g_spu_dma_rlog = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace-entries") && i + 1 < argc) { entries_file = argv[++i]; cpu_trace_entries_start(); }
        else if (!strcmp(argv[i], "--pc-hist")) g_pc_hist = calloc(RAM_SIZE / 4, 4);
    }
    if (!cue) { fprintf(stderr, "usage: %s --disc game.cue [--frames N] [--dump dir every]\n", argv[0]); return 1; }
    g_game_disc = disc_open_cue(cue);
    if (!g_game_disc) { fprintf(stderr, "cannot open %s\n", cue); return 1; }

    runtime_reset(g_game_disc, true);
    if (autoplay_file) {
        Disc *song = disc_open_audio_file(autoplay_file);
        if (!song) { fprintf(stderr, "cannot decode %s\n", autoplay_file); return 1; }
        autoplay_init(song);
        if (!boot_exe) boot_exe = "MAIN_G.EXE";
    }
    runtime_run(boot_exe);
    if (entries_file) cpu_trace_entries_dump(entries_file);
    learn_writes_dump();
    if (ram_dump_path) { FILE *rf = fopen(ram_dump_path, "wb"); if (rf) { fwrite(ram, 1, RAM_SIZE, rf); fclose(rf); } }
    if (wav_f) { wav_flush(); fclose(wav_f); }
    { extern u64 g_gte_hist[64]; fprintf(stderr, "GTE commands:"); for (int i = 0; i < 64; i++) if (g_gte_hist[i]) fprintf(stderr, " %02x:%llu", i, (unsigned long long)g_gte_hist[i]); fprintf(stderr, "\n"); }
    if (pc_hist_out && g_pc_hist) { FILE *hf = fopen(pc_hist_out, "w"); if (hf) { for (u32 i = 0; i < RAM_SIZE / 4; i++) if (g_pc_hist[i]) fprintf(hf, "%08x %u\n", 0x80000000u + i * 4, g_pc_hist[i]); fclose(hf); } }
    else print_hist();
    fprintf(stderr, "stopped after %d frames, %llu cycles\n", frame_no, (unsigned long long)cpu.cycles);
    return 0;
}
