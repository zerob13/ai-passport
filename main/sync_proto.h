// main/sync_proto.h —— BLE 同步协议编解码 + 日程/Todo 存储(纯 C,可主机测试)。
// 协议定义见 docs/software-design/passport-sync-app.md(中英双语)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_PROTO_VER      2        // 协议版本,HELLO.ver 携带
#define SYNC_MAX_TITLE      60       // 标题上限(UTF-8 字节)
#define SYNC_MAX_SCHED      40       // Imported schedule items kept in RAM
#define SYNC_SCHED_PAGE_SIZE 4       // Rows rendered on one schedule page
#define SYNC_MAX_TODO       32       // Todo 上限
#define SYNC_MAX_MEDIA_SOURCE 24     // 播放器名称上限(UTF-8 字节)
#define SYNC_MAX_PAYLOAD    240      // 单帧负载上限(兼容 MTU 247)
#define SYNC_AUDIO_DATA_MAX (SYNC_MAX_PAYLOAD - 2) // AUDIO_DATA minus seq u16
#define SYNC_FRAME_MAX      (3 + SYNC_MAX_PAYLOAD)
#define SYNC_MEDIA_ART_W    96
#define SYNC_MEDIA_ART_H    96
#define SYNC_MEDIA_ART_PIXELS (SYNC_MEDIA_ART_W * SYNC_MEDIA_ART_H)
#define SYNC_MEDIA_ART_BYTES  (SYNC_MEDIA_ART_PIXELS * 2)

// 帧: [0xA5][type][len][payload]  —— len = payload 长度,多字节字段小端序。

// RX(手机 → 设备)消息类型
#define SYNC_RX_HELLO           0x01 // ver u8, unix_time u32, tz_min i16
#define SYNC_RX_SCHEDULE_CLEAR  0x02 // Clear imported schedule data
#define SYNC_RX_SCHEDULE_ADD    0x03 // id, epoch_day, start, end, flags, title
#define SYNC_RX_TODO_CLEAR      0x05 // 清空 Todo
#define SYNC_RX_TODO_ADD        0x06 // id u16, done u8, title_len u8, title
#define SYNC_RX_MEDIA_CLEAR     0x08 // 清空 Now Playing
#define SYNC_RX_MEDIA_INFO      0x09 // flags, duration/position, title/artist/album/source
#define SYNC_RX_MEDIA_ART_BEGIN 0x0A // total_bytes u16
#define SYNC_RX_MEDIA_ART_DATA  0x0B // offset u16, RGB565 bytes
#define SYNC_RX_MEDIA_ART_END   0x0C // total_bytes u16
#define SYNC_RX_MEDIA_PROGRESS  0x0D // flags u8, position_ms u32, duration_ms u32

// TX(设备 → 手机)消息类型
#define SYNC_TX_AUDIO_START     0x10 // unix_time u32, sample_rate u16, codec u8, channels u8
#define SYNC_TX_AUDIO_DATA      0x11 // seq u16, data(ADPCM)
#define SYNC_TX_AUDIO_END       0x12 // duration_ms u32, pcm_samples u32, dropped_bytes u32
#define SYNC_TX_TODO_TOGGLE     0x20 // id u16, done u8
#define SYNC_TX_STATUS          0x30 // soc u8, flags u8, battery_mv u16

// 录音编码器标识(ADPCM_START.codec)
#define SYNC_CODEC_IMA_ADPCM    1

// 录音标志位(STATUS.flags)
#define SYNC_FLAG_RECORDING     0x01
#define SYNC_FLAG_CHARGING      0x02

// Now Playing flags.
#define SYNC_MEDIA_FLAG_PLAYING 0x01
#define SYNC_MEDIA_FLAG_HAS_ART 0x02

// Schedule item flags.
#define SYNC_SCHED_FLAG_ALL_DAY 0x01

typedef struct {
    uint16_t id;
    int32_t  epoch_day;          // Local calendar date as days since 1970-01-01
    uint16_t start_min;          // 当日 0 点起分钟数
    uint16_t end_min;
    uint8_t  flags;
    uint8_t  title_len;
    char     title[SYNC_MAX_TITLE + 1];
} sync_sched_item_t;

typedef struct {
    uint16_t id;
    uint8_t  done;               // 0/1
    uint8_t  title_len;
    char     title[SYNC_MAX_TITLE + 1];
} sync_todo_item_t;

typedef struct {
    bool     active;
    bool     playing;
    bool     has_art;
    bool     art_ready;
    uint32_t duration_ms;
    uint32_t position_ms;
    uint8_t  title_len;
    char     title[SYNC_MAX_TITLE + 1];
    uint8_t  artist_len;
    char     artist[SYNC_MAX_TITLE + 1];
    uint8_t  album_len;
    char     album[SYNC_MAX_TITLE + 1];
    uint8_t  source_len;
    char     source[SYNC_MAX_MEDIA_SOURCE + 1];
    uint16_t art_expected;
    uint16_t art_received;
    uint16_t art_rgb565[SYNC_MEDIA_ART_PIXELS];
} sync_media_t;

typedef struct {
    bool     time_set;
    uint32_t unix_time;          // 上次 HELLO 的时间(秒)
    int16_t  tz_min;             // UTC 东侧分钟数
    sync_sched_item_t sched[SYNC_MAX_SCHED];
    uint16_t sched_count;
    sync_todo_item_t  todos[SYNC_MAX_TODO];
    uint16_t todo_count;
    sync_media_t media;
} sync_store_t;

void sync_store_init(sync_store_t *st);

// 解析并应用一帧 RX 数据。返回:>0 = 已处理的类型;0 = 未知类型(忽略);
// <0 = 帧格式错误(帧头/长度非法)。
int sync_proto_rx(sync_store_t *st, const uint8_t *frame, size_t len);

// 构造一帧。返回帧总长;payload 超限或 out 容量不足返回 0。
size_t sync_proto_build(uint8_t type, const void *payload, size_t plen,
                        uint8_t *out, size_t cap);

// 各 TX 消息便捷构造器。返回帧长,失败返回 0。
size_t sync_proto_build_audio_start(uint8_t *out, size_t cap, uint32_t unix_time,
                                    uint16_t rate, uint8_t codec, uint8_t ch);
// data length must not exceed SYNC_AUDIO_DATA_MAX.
size_t sync_proto_build_audio_data(uint8_t *out, size_t cap, uint16_t seq,
                                   const uint8_t *data, size_t n);
size_t sync_proto_build_audio_end(uint8_t *out, size_t cap, uint32_t dur_ms,
                                  uint32_t pcm_samples, uint32_t dropped);
size_t sync_proto_build_todo_toggle(uint8_t *out, size_t cap, uint16_t id, uint8_t done);
size_t sync_proto_build_status(uint8_t *out, size_t cap, uint8_t soc,
                               uint8_t flags, uint16_t mv);

// A schedule always has at least one logical page, including the empty state.
uint16_t sync_sched_page_count(uint16_t total);
// Choose today's first item, otherwise the first future item or the final past item.
uint16_t sync_sched_default_page(const sync_store_t *st, int32_t today_epoch_day);
// Return the local epoch day for a POSIX timestamp and fixed timezone offset.
int32_t sync_proto_local_day(uint32_t unix_time, int16_t tz_min);
// Convert an epoch day to its Gregorian date.
void sync_proto_date_from_day(int32_t epoch_day, int *year, int *mon, int *day);

// 存储查询/变更(UI 与页面使用)。
const sync_sched_item_t *sync_sched_at(const sync_store_t *st, uint16_t idx);
const sync_todo_item_t  *sync_todo_at(const sync_store_t *st, uint16_t idx);

// 勾选/取消勾选:更新 id 对应项的 done,成功返回 true。
bool sync_todo_set_done(sync_store_t *st, uint16_t id, uint8_t done);

// 把 unix 秒 + 时区偏移换算为本地时间。任一年/月/日等指针可为 NULL。
// 算法为 Howard Hinnant 的 civil_from_days(纯 C,可主机测试)。
void sync_proto_local_time(uint32_t unix_time, int16_t tz_min,
                           int *year, int *mon, int *day,
                           int *hour, int *min);

// 音频帧序号自增(回绕)。
static inline uint16_t sync_seq_next(uint16_t *seq)
{
    uint16_t s = *seq;
    *seq = (uint16_t)(s + 1);
    return s;
}

#ifdef __cplusplus
}
#endif
