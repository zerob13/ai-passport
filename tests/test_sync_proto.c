// tests/test_sync_proto.c —— BLE 同步协议编解码与存储主机测试。
#include <assert.h>
#include <string.h>
#include "sync_proto.h"

static void test_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void test_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static size_t test_schedule_frame(uint8_t *frame, uint16_t id, int32_t epoch_day,
                                  uint16_t start_min, uint16_t end_min,
                                  uint8_t flags, const uint8_t *title,
                                  uint8_t title_len)
{
    frame[0] = 0xA5;
    frame[1] = SYNC_RX_SCHEDULE_ADD;
    frame[2] = (uint8_t)(12 + title_len);
    test_put_u16(frame + 3, id);
    test_put_u32(frame + 5, (uint32_t)epoch_day);
    test_put_u16(frame + 9, start_min);
    test_put_u16(frame + 11, end_min);
    frame[13] = flags;
    frame[14] = title_len;
    memcpy(frame + 15, title, title_len);
    return 15u + title_len;
}

int main(void)
{
    uint8_t frame[SYNC_FRAME_MAX];
    sync_store_t st;

    // ---- 帧构造 ----
    size_t n = sync_proto_build_audio_start(frame, sizeof(frame), 1700000000, 16000,
                                            SYNC_CODEC_IMA_ADPCM, 1);
    assert(n == 3 + 8);
    assert(frame[0] == 0xA5 && frame[1] == SYNC_TX_AUDIO_START && frame[2] == 8);
    assert(frame[3] == 0x00 && frame[7] == 0x80 && frame[8] == 0x3E);  // 小端 16000
    assert(frame[9] == SYNC_CODEC_IMA_ADPCM && frame[10] == 1);

    // AUDIO_DATA accepts the shared limit and rejects one byte over it.
    uint8_t ad[SYNC_AUDIO_DATA_MAX + 1];
    memset(ad, 0x5A, sizeof(ad));
    n = sync_proto_build_audio_data(frame, sizeof(frame), 0x0102,
                                    ad, SYNC_AUDIO_DATA_MAX);
    assert(n == 3 + 2 + SYNC_AUDIO_DATA_MAX);
    assert(frame[1] == SYNC_TX_AUDIO_DATA && frame[2] == 240);
    assert(frame[3] == 0x02 && frame[4] == 0x01 && frame[5] == 0x5A);
    assert(sync_proto_build_audio_data(frame, sizeof(frame), 0,
                                       ad, SYNC_AUDIO_DATA_MAX + 1) == 0);

    n = sync_proto_build_audio_end(frame, sizeof(frame), 65432, 1000000, 3);
    assert(n == 3 + 12);
    assert(frame[3] == 0x98 && frame[4] == 0xFF && frame[5] == 0x00 && frame[6] == 0x00);

    n = sync_proto_build_todo_toggle(frame, sizeof(frame), 7, 1);
    assert(n == 3 + 3 && frame[3] == 7 && frame[5] == 1);

    n = sync_proto_build_status(frame, sizeof(frame), 82, SYNC_FLAG_RECORDING, 3900);
    assert(n == 3 + 4 && frame[3] == 82 && frame[4] == 0x01);
    assert(frame[5] == 0x3C && frame[6] == 0x0F);   // 小端 3900

    // 容量不足失败
    uint8_t small[4];
    assert(sync_proto_build_audio_start(small, sizeof(small), 0, 16000, 1, 1) == 0);

    // ---- 序号自增 ----
    uint16_t seq = 0xFFFF;
    assert(sync_seq_next(&seq) == 0xFFFF);
    assert(seq == 0);

    // ---- RX 应用 ----
    sync_store_init(&st);
    assert(!st.time_set);
    uint8_t hello[10] = { 0xA5, SYNC_RX_HELLO, 7,
                          SYNC_PROTO_VER,                         // ver
                          0x00, 0xF1, 0x53, 0x65,                // unix_time 1700000000 (0x6553F100)
                          0xE0, 0x01 };                          // tz +480
    assert(sync_proto_rx(&st, hello, sizeof(hello)) == SYNC_RX_HELLO);
    assert(st.time_set && st.unix_time == 1700000000 && st.tz_min == 480);

    // Schedule items sort by date, all-day state, and start time.
    uint8_t clear[3] = { 0xA5, SYNC_RX_SCHEDULE_CLEAR, 0 };
    assert(sync_proto_rx(&st, clear, sizeof(clear)) == SYNC_RX_SCHEDULE_CLEAR);
    assert(st.sched_count == 0);

    uint8_t legacy_schedule[] = { 0xA5, SYNC_RX_SCHEDULE_ADD, 8,
                                  1, 0, 0, 0, 1, 0, 1, 'x' };
    assert(sync_proto_rx(&st, legacy_schedule, sizeof(legacy_schedule)) < 0);

    n = test_schedule_frame(frame, 2, 20700, 852, 940, 0,
                            (const uint8_t *)"Break", 5);
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    n = test_schedule_frame(frame, 1, 20699, 540, 600, 0,
                            (const uint8_t *)"Tea!", 4);
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    n = test_schedule_frame(frame, 3, 20699, 600, 660,
                            SYNC_SCHED_FLAG_ALL_DAY,
                            (const uint8_t *)"Holiday", 7);
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    assert(st.sched_count == 3);
    assert(st.sched[0].id == 3 && st.sched[0].start_min == 0);
    assert(st.sched[1].id == 1 && st.sched[1].start_min == 540);
    assert(st.sched[2].id == 2 && st.sched[2].epoch_day == 20700);

    // Replacing an id also repositions it.
    n = test_schedule_frame(frame, 2, 20699, 480, 600, 0,
                            (const uint8_t *)"Lunch", 5);
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    assert(st.sched_count == 3);
    assert(st.sched[1].id == 2 && st.sched[1].start_min == 480);
    assert(strcmp(st.sched[1].title, "Lunch") == 0);

    uint8_t long_title[70];
    memset(long_title, 'x', sizeof(long_title));
    n = test_schedule_frame(frame, 9, 20698, 1, 2, 0,
                            long_title, sizeof(long_title));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    assert(st.sched[0].title_len == SYNC_MAX_TITLE);
    assert(st.sched[0].title[SYNC_MAX_TITLE] == '\0');

    // UTF-8 truncation must drop an incomplete multibyte code point.
    uint8_t utf8_title[62];
    memset(utf8_title, 'a', 59);
    utf8_title[59] = 0xE4; utf8_title[60] = 0xB8; utf8_title[61] = 0xAD;
    n = test_schedule_frame(frame, 10, 20698, 2, 3, 0,
                            utf8_title, sizeof(utf8_title));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    const sync_sched_item_t *utf8_item = NULL;
    for (uint16_t i = 0; i < st.sched_count; i++) {
        if (st.sched[i].id == 10) utf8_item = &st.sched[i];
    }
    assert(utf8_item != NULL);
    assert(utf8_item->title_len == 59);
    assert(utf8_item->title[59] == '\0');

    // The device keeps forty complete items and pages locally.
    assert(sync_proto_rx(&st, clear, sizeof(clear)) == SYNC_RX_SCHEDULE_CLEAR);
    for (int i = 0; i < SYNC_MAX_SCHED + 5; i++) {
        n = test_schedule_frame(frame, (uint16_t)(100 + i), 20690 + i,
                                60, 120, 0, (const uint8_t *)"t", 1);
        assert(sync_proto_rx(&st, frame, n) == SYNC_RX_SCHEDULE_ADD);
    }
    assert(st.sched_count == SYNC_MAX_SCHED);
    assert(sync_sched_page_count(0) == 1);
    assert(sync_sched_page_count(SYNC_MAX_SCHED) == 10);
    assert(sync_sched_default_page(&st, 20700) == 2);
    assert(sync_sched_default_page(&st, 20750) == 9);

    // ---- Todo ----
    uint8_t tclear[3] = { 0xA5, SYNC_RX_TODO_CLEAR, 0 };
    assert(sync_proto_rx(&st, tclear, sizeof(tclear)) == SYNC_RX_TODO_CLEAR);
    assert(st.todo_count == 0);

    uint8_t t1[] = { 0xA5, SYNC_RX_TODO_ADD, 10,
                     0x03, 0x00, 0x00, 6,
                     'B', 'u', 'y', ' ', 'm', 'i', 'l', 'k' };  // 帧长字段与实际不符 → 拒绝
    // 上面的负载: id(2) + done(1) + len(1) + 8 字节标题,共 12 字节,但帧长声明为 10
    assert(sync_proto_rx(&st, t1, sizeof(t1)) < 0);
    assert(st.todo_count == 0);

    uint8_t t2[] = { 0xA5, SYNC_RX_TODO_ADD, 10,
                     0x03, 0x00, 0x00, 6,
                     'B', 'u', 'y', ' ', 'm', 'i' };
    uint8_t t3[] = { 0xA5, SYNC_RX_TODO_ADD, 10,
                     0x04, 0x00, 0x01, 6,
                     'C', 'a', 'l', 'l', ' ', 'B' };
    assert(sync_proto_rx(&st, t2, sizeof(t2)) == SYNC_RX_TODO_ADD);
    assert(sync_proto_rx(&st, t3, sizeof(t3)) == SYNC_RX_TODO_ADD);
    assert(st.todo_count == 2);
    assert(st.todos[0].id == 3 && st.todos[0].done == 0);
    assert(st.todos[1].id == 4 && st.todos[1].done == 1);

    // 同 id 更新
    uint8_t t2b[] = { 0xA5, SYNC_RX_TODO_ADD, 11,
                      0x03, 0x00, 0x01, 7,
                      'B', 'u', 'y', ' ', 'm', 'i', 'l' };
    assert(sync_proto_rx(&st, t2b, sizeof(t2b)) == SYNC_RX_TODO_ADD);
    assert(st.todo_count == 2 && st.todos[0].id == 3 && st.todos[0].done == 1);
    assert(strcmp(st.todos[0].title, "Buy mil") == 0);

    // 设备侧勾选回写
    assert(sync_todo_set_done(&st, 4, 0));
    assert(st.todos[1].done == 0);
    assert(!sync_todo_set_done(&st, 999, 1));
    assert(sync_todo_at(&st, 0) == &st.todos[0]);
    assert(sync_todo_at(&st, 5) == NULL);
    assert(sync_sched_at(&st, 0) == &st.sched[0]);
    assert(sync_sched_at(&st, 99) == NULL);

    // ---- Now Playing ----
    const char *media_text[] = { "Song", "Artist", "Album", "SPOTIFY" };
    uint8_t media_info[64] = { 0 };
    media_info[0] = SYNC_MEDIA_FLAG_PLAYING | SYNC_MEDIA_FLAG_HAS_ART;
    test_put_u32(media_info + 1, 240000);
    test_put_u32(media_info + 5, 12345);
    size_t media_len = 9;
    for (size_t i = 0; i < 4; i++) {
        size_t text_len = strlen(media_text[i]);
        media_info[media_len++] = (uint8_t)text_len;
        memcpy(media_info + media_len, media_text[i], text_len);
        media_len += text_len;
    }
    n = sync_proto_build(SYNC_RX_MEDIA_INFO, media_info, media_len,
                         frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_INFO);
    assert(st.media.active && st.media.playing && st.media.has_art);
    assert(!st.media.art_ready && st.media.duration_ms == 240000);
    assert(st.media.position_ms == 12345);
    assert(strcmp(st.media.title, "Song") == 0);
    assert(strcmp(st.media.artist, "Artist") == 0);
    assert(strcmp(st.media.album, "Album") == 0);
    assert(strcmp(st.media.source, "SPOTIFY") == 0);

    uint8_t art_begin[2];
    test_put_u16(art_begin, SYNC_MEDIA_ART_BYTES);
    n = sync_proto_build(SYNC_RX_MEDIA_ART_BEGIN, art_begin, sizeof(art_begin),
                         frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_ART_BEGIN);

    uint8_t art_payload[SYNC_MAX_PAYLOAD];
    test_put_u16(art_payload, 1);
    art_payload[2] = 0xAA;
    n = sync_proto_build(SYNC_RX_MEDIA_ART_DATA, art_payload, 3,
                         frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) < 0);
    n = sync_proto_build(SYNC_RX_MEDIA_ART_END, art_begin, sizeof(art_begin),
                         frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) < 0);
    assert(!st.media.art_ready);

    n = sync_proto_build(SYNC_RX_MEDIA_ART_BEGIN, art_begin, sizeof(art_begin),
                         frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_ART_BEGIN);
    for (uint16_t offset = 0; offset < SYNC_MEDIA_ART_BYTES;) {
        size_t chunk = SYNC_MEDIA_ART_BYTES - offset;
        if (chunk > SYNC_MAX_PAYLOAD - 2) chunk = SYNC_MAX_PAYLOAD - 2;
        test_put_u16(art_payload, offset);
        for (size_t i = 0; i < chunk; i++) {
            art_payload[2 + i] = (uint8_t)(offset + i);
        }
        n = sync_proto_build(SYNC_RX_MEDIA_ART_DATA, art_payload, 2 + chunk,
                             frame, sizeof(frame));
        assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_ART_DATA);
        offset = (uint16_t)(offset + chunk);
    }
    n = sync_proto_build(SYNC_RX_MEDIA_ART_END, art_begin, sizeof(art_begin),
                         frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_ART_END);
    assert(st.media.art_ready);
    assert(st.media.art_received == SYNC_MEDIA_ART_BYTES);
    const uint8_t *art_bytes = (const uint8_t *)st.media.art_rgb565;
    assert(art_bytes[0] == 0x00 && art_bytes[1] == 0x01);
    assert(art_bytes[SYNC_MEDIA_ART_BYTES - 1] == 0xFF);

    uint8_t media_progress[9] = { 0 };
    test_put_u32(media_progress + 1, 23000);
    test_put_u32(media_progress + 5, 240000);
    n = sync_proto_build(SYNC_RX_MEDIA_PROGRESS, media_progress,
                         sizeof(media_progress), frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_PROGRESS);
    assert(!st.media.playing && st.media.position_ms == 23000);

    n = sync_proto_build(SYNC_RX_MEDIA_CLEAR, NULL, 0, frame, sizeof(frame));
    assert(sync_proto_rx(&st, frame, n) == SYNC_RX_MEDIA_CLEAR);
    assert(!st.media.active && !st.media.art_ready);

    // ---- 异常帧 ----
    uint8_t bad_hdr[4] = { 0x00, 0x01, 1, 1 };
    assert(sync_proto_rx(&st, bad_hdr, sizeof(bad_hdr)) < 0);
    uint8_t bad_len[4] = { 0xA5, SYNC_RX_HELLO, 7, 1 };          // 长度与负载不符
    assert(sync_proto_rx(&st, bad_len, sizeof(bad_len)) < 0);
    uint8_t unknown[4] = { 0xA5, 0x7F, 1, 0 };                   // 未知类型:忽略
    assert(sync_proto_rx(&st, unknown, sizeof(unknown)) == 0);
    uint8_t hello_short[3 + 6] = { 0xA5, SYNC_RX_HELLO, 6 };     // HELLO 负载长度错误
    assert(sync_proto_rx(&st, hello_short, sizeof(hello_short)) < 0);
    uint8_t hello_v1[10] = { 0xA5, SYNC_RX_HELLO, 7, 1 };
    assert(sync_proto_rx(&st, hello_v1, sizeof(hello_v1)) < 0);

    // ---- 本地时间换算 ----
    int y, m, d, h, mi;
    sync_proto_local_time(0, 0, &y, &m, &d, &h, &mi);            // epoch UTC
    assert(y == 1970 && m == 1 && d == 1 && h == 0 && mi == 0);
    sync_proto_local_time(1700000000, 480, &y, &m, &d, &h, &mi); // 2023-11-14 22:13 UTC +8
    assert(y == 2023 && m == 11 && d == 15 && h == 6 && mi == 13);
    sync_proto_local_time(1700000000, 0, &y, &m, &d, &h, &mi);
    assert(y == 2023 && m == 11 && d == 14 && h == 22 && mi == 13);
    sync_proto_local_time(951782400, 0, &y, &m, &d, &h, &mi);    // 2000-02-29 闰日 UTC
    assert(y == 2000 && m == 2 && d == 29);
    assert(sync_proto_local_day(1700000000, 480) == 19676);
    sync_proto_date_from_day(19676, &y, &m, &d);
    assert(y == 2023 && m == 11 && d == 15);

    return 0;
}
