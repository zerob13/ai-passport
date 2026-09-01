// tests/test_sync_proto.c —— BLE 同步协议编解码与存储主机测试。
#include <assert.h>
#include <string.h>
#include "sync_proto.h"

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
                          0x01,                                  // ver
                          0x00, 0xF1, 0x53, 0x65,                // unix_time 1700000000 (0x6553F100)
                          0xE0, 0x01 };                          // tz +480
    assert(sync_proto_rx(&st, hello, sizeof(hello)) == SYNC_RX_HELLO);
    assert(st.time_set && st.unix_time == 1700000000 && st.tz_min == 480);

    // 清空 + 添加日程(乱序,应按时段排序)
    uint8_t clear[3] = { 0xA5, SYNC_RX_SCHEDULE_CLEAR, 0 };
    assert(sync_proto_rx(&st, clear, sizeof(clear)) == SYNC_RX_SCHEDULE_CLEAR);
    assert(st.sched_count == 0);

    uint8_t add1[] = { 0xA5, SYNC_RX_SCHEDULE_ADD, 12,
                       0x02, 0x00, 0x54, 0x03, 0xAC, 0x03, 5,
                       'B', 'r', 'e', 'a', 'k' };                // id=2 14:12~15:40
    uint8_t add2[] = { 0xA5, SYNC_RX_SCHEDULE_ADD, 11,
                       0x01, 0x00, 0x1C, 0x02, 0x58, 0x02, 4,
                       'T', 'e', 'a', '!' };                     // id=1 09:00~10:00
    assert(sync_proto_rx(&st, add1, sizeof(add1)) == SYNC_RX_SCHEDULE_ADD);
    assert(sync_proto_rx(&st, add2, sizeof(add2)) == SYNC_RX_SCHEDULE_ADD);
    assert(st.sched_count == 2);
    assert(st.sched[0].id == 1 && st.sched[0].start_min == 540);
    assert(st.sched[1].id == 2 && st.sched[1].start_min == 852);
    assert(strcmp(st.sched[0].title, "Tea!") == 0);
    assert(st.sched[0].title_len == 4);

    // 同 id 替换,条数不变
    uint8_t add2b[] = { 0xA5, SYNC_RX_SCHEDULE_ADD, 12,
                        0x02, 0x00, 0x20, 0x03, 0xC0, 0x03, 5,
                        'L', 'u', 'n', 'c', 'h' };               // id=2 改为 13:20~16:00
    assert(sync_proto_rx(&st, add2b, sizeof(add2b)) == SYNC_RX_SCHEDULE_ADD);
    assert(st.sched_count == 2);
    assert(st.sched[1].id == 2 && st.sched[1].start_min == 800);
    assert(strcmp(st.sched[1].title, "Lunch") == 0);

    // 标题超长:截断到 60 字节,不丢弃
    uint8_t add_long[3 + 7 + 70];
    add_long[0] = 0xA5; add_long[1] = SYNC_RX_SCHEDULE_ADD; add_long[2] = 7 + 70;
    add_long[3] = 9; add_long[4] = 0;                            // id=9
    add_long[5] = 0; add_long[6] = 0; add_long[7] = 0; add_long[8] = 0;
    add_long[9] = 70;
    memset(add_long + 10, 'x', 70);
    assert(sync_proto_rx(&st, add_long, sizeof(add_long)) == SYNC_RX_SCHEDULE_ADD);
    assert(st.sched_count == 3);
    // start_min=0 排最前(id=9 的长标题项)
    assert(st.sched[0].title_len == SYNC_MAX_TITLE);
    assert(st.sched[0].title[SYNC_MAX_TITLE] == '\0');

    // 容量上限:满 32 条后丢弃
    for (int i = 0; i < 40; i++) {
        uint8_t a[3 + 7 + 1];
        a[0] = 0xA5; a[1] = SYNC_RX_SCHEDULE_ADD; a[2] = 8;
        a[3] = (uint8_t)(100 + i); a[4] = 0;
        a[5] = (uint8_t)i; a[6] = 0; a[7] = (uint8_t)i; a[8] = 0;
        a[9] = 1; a[10] = 't';
        assert(sync_proto_rx(&st, a, sizeof(a)) == SYNC_RX_SCHEDULE_ADD);
    }
    assert(st.sched_count == SYNC_MAX_SCHED);

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

    // ---- 异常帧 ----
    uint8_t bad_hdr[4] = { 0x00, 0x01, 1, 1 };
    assert(sync_proto_rx(&st, bad_hdr, sizeof(bad_hdr)) < 0);
    uint8_t bad_len[4] = { 0xA5, SYNC_RX_HELLO, 7, 1 };          // 长度与负载不符
    assert(sync_proto_rx(&st, bad_len, sizeof(bad_len)) < 0);
    uint8_t unknown[4] = { 0xA5, 0x7F, 1, 0 };                   // 未知类型:忽略
    assert(sync_proto_rx(&st, unknown, sizeof(unknown)) == 0);
    uint8_t hello_short[3 + 6] = { 0xA5, SYNC_RX_HELLO, 6 };     // HELLO 负载长度错误
    assert(sync_proto_rx(&st, hello_short, sizeof(hello_short)) < 0);

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

    return 0;
}
