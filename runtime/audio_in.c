/* Decode any audio file to 44.1 kHz stereo s16 and wrap it as a one-track audio CD.
 * miniaudio handles wav/flac/mp3/ogg; anything else (m4a, opus, ...) goes through ffmpeg if available.
 * (The planned Rust frontend decodes with Symphonia and calls disc_open_audio_pcm directly.) */
#include "psx.h"
#include <stdlib.h>

#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include "../crates/audioout/c/miniaudio.h"

#define MAX_FRAMES (44100ull * 60 * 80)      /* an 80 minute CD */

static s16 *decode_miniaudio(const char *path, size_t *nframes) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 2, 44100);
    ma_decoder dec;
    if (ma_decoder_init_file(path, &cfg, &dec) != MA_SUCCESS) return NULL;
    size_t cap = 44100 * 60, n = 0;
    s16 *buf = malloc(cap * 4);
    for (;;) {
        if (n + 8192 > cap) {
            cap *= 2;
            if (cap > MAX_FRAMES) cap = MAX_FRAMES;
            if (n + 8192 > cap) break;
            buf = realloc(buf, cap * 4);
        }
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, buf + n * 2, 8192, &got);
        n += got;
        if (got == 0 || r != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&dec);
    if (n == 0) { free(buf); return NULL; }
    *nframes = n;
    return buf;
}

static s16 *decode_ffmpeg(const char *path, size_t *nframes) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -nostdin -i \"%s\" -f s16le -ac 2 -ar 44100 - 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 44100 * 60, n = 0;
    s16 *buf = malloc(cap * 4);
    for (;;) {
        if (n + 8192 > cap) { cap *= 2; buf = realloc(buf, cap * 4); }
        size_t got = fread(buf + n * 2, 4, 8192, p);
        n += got;
        if (got == 0 || n >= MAX_FRAMES) break;
    }
    pclose(p);
    if (n == 0) { free(buf); return NULL; }
    *nframes = n;
    return buf;
}

Disc *disc_open_audio_file(const char *path) {
    size_t n = 0;
    s16 *pcm = decode_miniaudio(path, &n);
    if (!pcm) pcm = decode_ffmpeg(path, &n);
    if (!pcm) return NULL;
    const char *base = strrchr(path, '/');
    Disc *d = disc_open_audio_pcm(pcm, n, base ? base + 1 : path);
    free(pcm);
    return d;
}
