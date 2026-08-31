// main/sync_proto.c —— 协议编解码与存储实现(纯 C)。见 sync_proto.h。
#include "sync_proto.h"

#include <string.h>

// ---- 小端读写 ----
static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int16_t get_i16(const uint8_t *p) { return (int16_t)get_u16(p); }

void sync_store_init(sync_store_t *st)
{
    memset(st, 0, sizeof(*st));
}

// 标题拷贝:截断到 SYNC_MAX_TITLE 字节(协议层只截断,不拒绝)。
static void copy_title(char *dst, uint8_t *len, const uint8_t *src, uint8_t n)
{
    uint8_t m = n < SYNC_MAX_TITLE ? n : SYNC_MAX_TITLE;
    memcpy(dst, src, m);
    dst[m] = '\0';
    *len = m;
}

// 日程:按 id 替换或插入,并按 start_min 升序稳定插入。
static void sched_upsert(sync_store_t *st, uint16_t id, uint16_t start_min,
                         uint16_t end_min, const uint8_t *title, uint8_t title_len)
{
    for (uint16_t i = 0; i < st->sched_count; i++) {
        if (st->sched[i].id == id) {
            st->sched[i].start_min = start_min;
            st->sched[i].end_min = end_min;
            copy_title(st->sched[i].title, &st->sched[i].title_len, title, title_len);
            return;
        }
    }
    if (st->sched_count >= SYNC_MAX_SCHED) return;
    uint16_t i = st->sched_count;
    while (i > 0 && st->sched[i - 1].start_min > start_min) {
        st->sched[i] = st->sched[i - 1];
        i--;
    }
    st->sched[i].id = id;
    st->sched[i].start_min = start_min;
    st->sched[i].end_min = end_min;
    copy_title(st->sched[i].title, &st->sched[i].title_len, title, title_len);
    st->sched_count++;
}

// Todo:按 id 替换或追加。
static void todo_upsert(sync_store_t *st, uint16_t id, uint8_t done,
                        const uint8_t *title, uint8_t title_len)
{
    for (uint16_t i = 0; i < st->todo_count; i++) {
        if (st->todos[i].id == id) {
            st->todos[i].done = done;
            copy_title(st->todos[i].title, &st->todos[i].title_len, title, title_len);
            return;
        }
    }
    if (st->todo_count >= SYNC_MAX_TODO) return;
    st->todos[st->todo_count].id = id;
    st->todos[st->todo_count].done = done;
    copy_title(st->todos[st->todo_count].title, &st->todos[st->todo_count].title_len,
               title, title_len);
    st->todo_count++;
}

static int rx_schedule_add(sync_store_t *st, const uint8_t *p, size_t n)
{
    if (n < 7) return -1;                       // id2 + start2 + end2 + len1
    uint8_t tlen = p[6];
    if (n != 7u + tlen) return -1;
    sched_upsert(st, get_u16(p), get_u16(p + 2), get_u16(p + 4), p + 7, tlen);
    return SYNC_RX_SCHEDULE_ADD;
}

static int rx_todo_add(sync_store_t *st, const uint8_t *p, size_t n)
{
    if (n < 4) return -1;                       // id2 + done1 + len1
    uint8_t tlen = p[3];
    if (n != 4u + tlen) return -1;
    todo_upsert(st, get_u16(p), p[2], p + 4, tlen);
    return SYNC_RX_TODO_ADD;
}

int sync_proto_rx(sync_store_t *st, const uint8_t *frame, size_t len)
{
    if (len < 3 || frame[0] != 0xA5) return -1;
    size_t plen = frame[2];
    if (len != 3 + plen) return -2;
    const uint8_t *p = frame + 3;
    switch (frame[1]) {
    case SYNC_RX_HELLO:
        if (plen != 7) return -1;
        st->time_set = true;
        st->unix_time = get_u32(p + 1);
        st->tz_min = get_i16(p + 5);
        return SYNC_RX_HELLO;
    case SYNC_RX_SCHEDULE_CLEAR:
        if (plen != 0) return -1;
        st->sched_count = 0;
        return SYNC_RX_SCHEDULE_CLEAR;
    case SYNC_RX_SCHEDULE_ADD:
        return rx_schedule_add(st, p, plen);
    case SYNC_RX_TODO_CLEAR:
        if (plen != 0) return -1;
        st->todo_count = 0;
        return SYNC_RX_TODO_CLEAR;
    case SYNC_RX_TODO_ADD:
        return rx_todo_add(st, p, plen);
    default:
        return 0;                               // 未知类型:忽略
    }
}

size_t sync_proto_build(uint8_t type, const void *payload, size_t plen,
                        uint8_t *out, size_t cap)
{
    if (plen > SYNC_MAX_PAYLOAD || cap < 3 + plen) return 0;
    out[0] = 0xA5;
    out[1] = type;
    out[2] = (uint8_t)plen;
    if (plen) memcpy(out + 3, payload, plen);
    return 3 + plen;
}

size_t sync_proto_build_audio_start(uint8_t *out, size_t cap, uint32_t unix_time,
                                    uint16_t rate, uint8_t codec, uint8_t ch)
{
    uint8_t p[8];
    put_u32(p, unix_time);
    put_u16(p + 4, rate);
    p[6] = codec;
    p[7] = ch;
    return sync_proto_build(SYNC_TX_AUDIO_START, p, sizeof(p), out, cap);
}

size_t sync_proto_build_audio_data(uint8_t *out, size_t cap, uint16_t seq,
                                   const uint8_t *data, size_t n)
{
    if (n > SYNC_MAX_PAYLOAD - 2) return 0;
    // 帧头 + seq(2B) + data
    if (cap < 3 + 2 + n) return 0;
    out[0] = 0xA5;
    out[1] = SYNC_TX_AUDIO_DATA;
    out[2] = (uint8_t)(2 + n);
    put_u16(out + 3, seq);
    if (n) memcpy(out + 5, data, n);
    return 3 + 2 + n;
}

size_t sync_proto_build_audio_end(uint8_t *out, size_t cap, uint32_t dur_ms,
                                  uint32_t pcm_samples, uint32_t dropped)
{
    uint8_t p[12];
    put_u32(p, dur_ms);
    put_u32(p + 4, pcm_samples);
    put_u32(p + 8, dropped);
    return sync_proto_build(SYNC_TX_AUDIO_END, p, sizeof(p), out, cap);
}

size_t sync_proto_build_todo_toggle(uint8_t *out, size_t cap, uint16_t id, uint8_t done)
{
    uint8_t p[3];
    put_u16(p, id);
    p[2] = done;
    return sync_proto_build(SYNC_TX_TODO_TOGGLE, p, sizeof(p), out, cap);
}

size_t sync_proto_build_status(uint8_t *out, size_t cap, uint8_t soc,
                               uint8_t flags, uint16_t mv)
{
    uint8_t p[4];
    p[0] = soc;
    p[1] = flags;
    put_u16(p + 2, mv);
    return sync_proto_build(SYNC_TX_STATUS, p, sizeof(p), out, cap);
}

const sync_sched_item_t *sync_sched_at(const sync_store_t *st, uint16_t idx)
{
    if (idx >= st->sched_count) return NULL;
    return &st->sched[idx];
}

const sync_todo_item_t *sync_todo_at(const sync_store_t *st, uint16_t idx)
{
    if (idx >= st->todo_count) return NULL;
    return &st->todos[idx];
}

bool sync_todo_set_done(sync_store_t *st, uint16_t id, uint8_t done)
{
    for (uint16_t i = 0; i < st->todo_count; i++) {
        if (st->todos[i].id == id) {
            st->todos[i].done = done ? 1 : 0;
            return true;
        }
    }
    return false;
}

// civil_from_days:天数(1970-01-01 起) → 公历 y/m/d。
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    yy += (mm <= 2);
    *y = (int)yy;
    *m = (int)mm;
    *d = (int)dd;
}

void sync_proto_local_time(uint32_t unix_time, int16_t tz_min,
                           int *year, int *mon, int *day,
                           int *hour, int *min)
{
    int64_t secs = (int64_t)unix_time + (int64_t)tz_min * 60;
    int64_t days = secs / 86400;
    if (secs < 0) days--;                 // C 向零取整,负秒修正
    int64_t rem = secs - days * 86400;
    int hh = (int)(rem / 3600);
    int mn = (int)((rem % 3600) / 60);
    int yy = 0, mm = 0, dd = 0;
    civil_from_days(days, &yy, &mm, &dd);
    if (year) *year = yy;
    if (mon) *mon = mm;
    if (day) *day = dd;
    if (hour) *hour = hh;
    if (min) *min = mn;
}