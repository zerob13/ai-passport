#include "app_chime.h"

#ifndef APP_CHIME_SYNTH_ONLY
#include "bsp_audio.h"
#endif

typedef struct {
    uint16_t hz[3];
    uint16_t duration_ms;
} chord_t;

static const int16_t SINE[64] = {
         0,   3212,   6393,   9512,  12539,  15446,  18204,  20787,
     23170,  25329,  27245,  28898,  30273,  31356,  32137,  32609,
     32767,  32609,  32137,  31356,  30273,  28898,  27245,  25329,
     23170,  20787,  18204,  15446,  12539,   9512,   6393,   3212,
         0,  -3212,  -6393,  -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
    -32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539,  -9512,  -6393,  -3212,
};

// Original short system chimes: ascending on startup, descending on shutdown.
static const chord_t STARTUP[] = {
    { { 262, 330, 392 }, 150 },
    { { 330, 392, 494 }, 170 },
    { { 392, 494, 587 }, 320 },
};
static const chord_t SHUTDOWN[] = {
    { { 440, 554, 659 }, 160 },
    { { 349, 440, 523 }, 160 },
    { { 262, 330, 392 }, 300 },
};

static const chord_t *sequence(app_chime_t chime, size_t *count)
{
    if (chime == APP_CHIME_STARTUP) {
        *count = sizeof(STARTUP) / sizeof(STARTUP[0]);
        return STARTUP;
    }
    if (chime == APP_CHIME_SHUTDOWN) {
        *count = sizeof(SHUTDOWN) / sizeof(SHUTDOWN[0]);
        return SHUTDOWN;
    }
    *count = 0;
    return NULL;
}

size_t app_chime_sample_count(app_chime_t chime)
{
    size_t count;
    const chord_t *notes = sequence(chime, &count);
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += (size_t)notes[i].duration_ms * APP_CHIME_SAMPLE_RATE / 1000;
    }
    return total;
}

static int16_t render_sample(const chord_t *chord, size_t position,
                             size_t chord_samples)
{
    const size_t attack = APP_CHIME_SAMPLE_RATE * 25 / 1000;
    const size_t release = APP_CHIME_SAMPLE_RATE * 60 / 1000;
    size_t remaining = chord_samples - position - 1;
    int32_t envelope = 32767;
    if (position < attack) envelope = (int32_t)(position * 32767 / attack);
    if (remaining < release) {
        int32_t tail = (int32_t)(remaining * 32767 / release);
        if (tail < envelope) envelope = tail;
    }

    int32_t mixed = 0;
    for (size_t voice = 0; voice < 3; voice++) {
        size_t phase = ((uint64_t)position * chord->hz[voice] * 64 /
                        APP_CHIME_SAMPLE_RATE) & 63;
        mixed += SINE[phase];
    }
    return (int16_t)((mixed / 8) * envelope / 32767);
}

size_t app_chime_render(app_chime_t chime, size_t offset,
                        int16_t *output, size_t capacity)
{
    if (!output || capacity == 0) return 0;

    size_t count;
    const chord_t *notes = sequence(chime, &count);
    size_t total = app_chime_sample_count(chime);
    if (!notes || offset >= total) return 0;

    size_t written = 0;
    size_t sequence_offset = 0;
    for (size_t i = 0; i < count && written < capacity; i++) {
        size_t chord_samples = (size_t)notes[i].duration_ms *
                               APP_CHIME_SAMPLE_RATE / 1000;
        if (offset >= sequence_offset + chord_samples) {
            sequence_offset += chord_samples;
            continue;
        }
        size_t position = offset > sequence_offset ? offset - sequence_offset : 0;
        while (position < chord_samples && written < capacity) {
            output[written++] = render_sample(&notes[i], position, chord_samples);
            position++;
            offset++;
        }
        sequence_offset += chord_samples;
    }
    return written;
}

#ifndef APP_CHIME_SYNTH_ONLY
bool app_chime_play(app_chime_t chime)
{
    if (bsp_audio_set_format(APP_CHIME_SAMPLE_RATE, 16, 2) != ESP_OK) return false;
    bsp_audio_set_volume(100);

    int16_t mono[128];
    int16_t stereo[256];
    size_t offset = 0;
    size_t count;
    while ((count = app_chime_render(chime, offset, mono,
                                     sizeof(mono) / sizeof(mono[0]))) > 0) {
        for (size_t i = 0; i < count; i++) {
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        if (bsp_audio_write(stereo, count * 2 * sizeof(stereo[0])) != ESP_OK) {
            return false;
        }
        offset += count;
    }
    return offset == app_chime_sample_count(chime);
}
#endif
