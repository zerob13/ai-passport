#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CHIME_SAMPLE_RATE 16000

typedef enum {
    APP_CHIME_STARTUP = 0,
    APP_CHIME_SHUTDOWN,
} app_chime_t;

size_t app_chime_sample_count(app_chime_t chime);
size_t app_chime_render(app_chime_t chime, size_t offset,
                        int16_t *output, size_t capacity);
bool app_chime_play(app_chime_t chime);

#ifdef __cplusplus
}
#endif
