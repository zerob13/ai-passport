#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "app_chime.h"

static int peak(const int16_t *samples, size_t count)
{
    int result = 0;
    for (size_t i = 0; i < count; i++) {
        int value = abs(samples[i]);
        if (value > result) result = value;
    }
    return result;
}

int main(void)
{
    static int16_t startup[APP_CHIME_SAMPLE_RATE];
    static int16_t shutdown[APP_CHIME_SAMPLE_RATE];
    static int16_t chunked[APP_CHIME_SAMPLE_RATE];
    size_t startup_count = app_chime_sample_count(APP_CHIME_STARTUP);
    size_t shutdown_count = app_chime_sample_count(APP_CHIME_SHUTDOWN);

    assert(startup_count == 10240);
    assert(shutdown_count == 9920);
    assert(app_chime_render(APP_CHIME_STARTUP, 0, startup, startup_count) ==
           startup_count);
    assert(app_chime_render(APP_CHIME_SHUTDOWN, 0, shutdown, shutdown_count) ==
           shutdown_count);
    assert(startup[0] == 0 && startup[startup_count - 1] == 0);
    assert(shutdown[0] == 0 && shutdown[shutdown_count - 1] == 0);
    assert(peak(startup, startup_count) > 3000);
    assert(peak(startup, startup_count) < 16000);
    assert(memcmp(startup, shutdown, shutdown_count * sizeof(int16_t)) != 0);

    size_t offset = 0;
    while (offset < startup_count) {
        size_t count = app_chime_render(APP_CHIME_STARTUP, offset,
                                        chunked + offset, 137);
        assert(count > 0);
        offset += count;
    }
    assert(memcmp(startup, chunked, startup_count * sizeof(int16_t)) == 0);
    assert(app_chime_render(APP_CHIME_STARTUP, startup_count, chunked, 1) == 0);
    return 0;
}
