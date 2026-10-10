/* Thin wrapper around miniaudio's playback device: fixed 44.1 kHz stereo s16, pulled through a callback.
 * miniaudio converts to whatever rate the device really runs at. */
#include <stdint.h>
#include <stdlib.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

typedef void (*audioout_fill_fn)(void *user, int16_t *out, uint32_t frames);

typedef struct {
    ma_device dev;
    audioout_fill_fn fill;
    void *user;
} AudioOut;

static void data_cb(ma_device *d, void *out, const void *in, ma_uint32 frames) {
    (void)in;
    AudioOut *a = (AudioOut *)d->pUserData;
    a->fill(a->user, (int16_t *)out, frames);
}

/* Returns NULL on failure; *err gets the miniaudio result code. */
AudioOut *audioout_open(audioout_fill_fn fill, void *user, int *err) {
    AudioOut *a = (AudioOut *)calloc(1, sizeof *a);
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = 44100;
    cfg.dataCallback = data_cb;
    cfg.pUserData = a;
    cfg.periodSizeInMilliseconds = 10;
    cfg.periods = 3;
    a->fill = fill;
    a->user = user;
    ma_result r = ma_device_init(NULL, &cfg, &a->dev);
    if (r == MA_SUCCESS) r = ma_device_start(&a->dev);
    if (r != MA_SUCCESS) {
        if (err) *err = (int)r;
        ma_device_uninit(&a->dev);
        free(a);
        return NULL;
    }
    return a;
}

void audioout_close(AudioOut *a) {
    if (!a) return;
    ma_device_uninit(&a->dev);
    free(a);
}
