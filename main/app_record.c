// main/app_record.c —— 录音页。
// 录音流程:OK 开始 → 录音任务按 30ms 块读 I2S(16k/16bit/mono) →
// IMA ADPCM 4:1 编码 → BLE notify 实时上传 → OK 停止/退出时 AUDIO_END 定稿。
// 上/下键在本页无操作(v1)。录音中离开页面(双击/长按)会先停止并定稿。
//
// 并发约定:页面回调(enter/exit/key/refresh)运行在 LVGL 持锁上下文,直接操作
// lv_*;录音任务里的 set_state() 自己加锁。退出时把 s_scr 置空,任务端
// set_state 有 s_scr 守卫,不会碰到已删除的对象。
#include "app_record.h"

#include "adpcm_ima.h"
#include "app_pager.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "sync_ble.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdlib.h>

static const char *TAG = "app_record";

#define REC_HZ        16000
#define CHUNK_SAMPLES 480           // 30ms/块 → 240B ADPCM,一条通知一帧
#define ENC_BYTES     (CHUNK_SAMPLES / 2)

// 录音任务命令 / 会话命令
enum {
    REC_CMD_NONE = 0,
    REC_CMD_START,                  // 开始录音(需已连接)
    REC_CMD_STOP,                   // 停止并定稿(会话循环直接读)
    REC_CMD_QUIT,                   // 退出任务
};

static lv_obj_t *s_scr;
static lv_obj_t *s_bat;
static lv_obj_t *s_dot;             // REC 指示灯
static lv_obj_t *s_time_lbl;        // 已录时长
static lv_obj_t *s_state_lbl;       // 状态行
static lv_obj_t *s_file_lbl;        // 文件名行
static lv_obj_t *s_foot_lbl;        // 底部连接状态
static lv_obj_t *s_mode_lbl;
static lv_obj_t *s_action_lbl;
static lv_timer_t *s_timer;         // 1s 刷新时长/指示灯

static TaskHandle_t s_task;
static SemaphoreHandle_t s_sem;     // 唤醒任务(开始/退出)
static volatile int s_cmd;          // 任务命令(START/QUIT)
static volatile int s_session_cmd;  // 会话命令(录音循环每块读一次)
static volatile bool s_recording;   // 录音中(任务回写)
static volatile uint32_t s_samples; // 已录 PCM 采样数(UI 读)
static bool s_audio_ok;             // codec 可用性

// 仅供录音任务调用(非 LVGL 上下文),内部加锁;页面回调不要用。
static void set_state(const char *text)
{
    if (!bsp_lvgl_lock(500)) return;
    if (s_scr && s_state_lbl) lv_label_set_text(s_state_lbl, text);
    bsp_lvgl_unlock();
}

// ============================================================================
// 录音任务
// ============================================================================

static void record_session(void)
{
    if (bsp_audio_set_format(REC_HZ, 16, 1) != ESP_OK) {
        set_state("MIC UNAVAILABLE");
        s_recording = false;
        return;
    }

    int16_t *pcm = malloc(CHUNK_SAMPLES * sizeof(int16_t));
    uint8_t *enc = malloc(ENC_BYTES);
    uint8_t *frame = malloc(SYNC_FRAME_MAX);
    if (!pcm || !enc || !frame) {
        free(pcm); free(enc); free(frame);
        set_state("MEMORY FULL");
        s_recording = false;
        return;
    }

    adpcm_ima_t ad;
    adpcm_ima_init(&ad);
    uint16_t seq = 0;
    uint32_t dropped = 0;
    uint32_t samples = 0;
    bool abnormal = false;

    s_recording = true;
    sync_ble_set_recording(true);
    s_samples = 0;
    s_session_cmd = REC_CMD_NONE;

    // 开始帧:带设备当前时间,手机用它生成文件名
    uint32_t now = 0;
    sync_ble_now(&now);
    size_t n = sync_proto_build_audio_start(frame, SYNC_FRAME_MAX,
                                            now, REC_HZ, SYNC_CODEC_IMA_ADPCM, 1);
    if (sync_ble_send(frame, n) != ESP_OK) {
        set_state("UPLOAD FAILED");
        sync_ble_set_recording(false);
        s_recording = false;
        free(pcm); free(enc); free(frame);
        return;
    }
    set_state("RECORDING");

    while (s_session_cmd == REC_CMD_NONE) {
        if (bsp_audio_read(pcm, CHUNK_SAMPLES * sizeof(int16_t)) != ESP_OK) {
            abnormal = true;
            break;
        }
        samples += CHUNK_SAMPLES;
        s_samples = samples;
        adpcm_ima_encode(&ad, pcm, CHUNK_SAMPLES, enc);
        n = sync_proto_build_audio_data(frame, SYNC_FRAME_MAX, seq, enc, ENC_BYTES);
        seq = (uint16_t)(seq + 1);
        esp_err_t r = sync_ble_send(frame, n);
        if (r == ESP_ERR_INVALID_STATE) {    // 断链/未订阅:无法继续
            abnormal = true;
            break;
        }
        if (r != ESP_OK) dropped += (uint32_t)ENC_BYTES;   // 拥塞丢块,计入定稿帧
    }

    // 定稿
    n = sync_proto_build_audio_end(frame, SYNC_FRAME_MAX,
                                   (uint32_t)(samples * 1000 / REC_HZ),
                                   samples, dropped);
    sync_ble_send(frame, n);
    sync_ble_set_recording(false);
    s_recording = false;

    if (abnormal) {
        set_state("LINK LOST");
    } else {
        ESP_LOGI(TAG, "录音结束 %u 秒,丢 %u B",
                 (unsigned)(samples / REC_HZ), (unsigned)dropped);
        set_state("SAVED");
    }
    free(pcm); free(enc); free(frame);
}

static void record_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        int cmd = s_cmd;
        s_cmd = REC_CMD_NONE;
        if (cmd == REC_CMD_QUIT) break;
        if (cmd == REC_CMD_START) {
            if (!s_audio_ok) {
                set_state("MIC UNAVAILABLE");
                continue;
            }
            record_session();
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// UI
// ============================================================================

static lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t color, int border, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(obj, border, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void reel_create(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *outer = shape(parent, x, y, 50, 50, UI_SURFACE, 2,
                            LV_RADIUS_CIRCLE);
    lv_obj_t *inner = shape(outer, 9, 9, 28, 28, UI_LINE, 0,
                            LV_RADIUS_CIRCLE);
    shape(inner, 12, 2, 4, 24, UI_INK, 0, 0);
    shape(inner, 2, 12, 24, 4, UI_INK, 0, 0);
    shape(inner, 9, 9, 10, 10, UI_SURFACE, 0, LV_RADIUS_CIRCLE);
    shape(inner, 12, 12, 4, 4, UI_INK, 0, LV_RADIUS_CIRCLE);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t secs = (uint32_t)(s_samples / REC_HZ);
    lv_label_set_text_fmt(s_time_lbl, "%02u:%02u",
                          (unsigned)(secs / 60), (unsigned)(secs % 60));
    if (s_recording) {
        static int phase;                       // REC 灯闪烁
        phase = (phase + 1) % 4;
        lv_obj_set_style_opa(s_dot, phase < 2 ? LV_OPA_COVER : LV_OPA_20, 0);
    } else {
        lv_obj_set_style_opa(s_dot, LV_OPA_TRANSP, 0);
    }
    lv_label_set_text(s_mode_lbl, s_recording ? "LIVE" : "READY");
    lv_label_set_text(s_action_lbl, s_recording ? "OK  STOP" : "OK  START");
}

static void file_label(void)
{
    uint32_t now = 0;
    if (sync_ble_now(&now) && s_file_lbl) {
        int y, mo, d, h, mi;
        sync_proto_local_time(now, app_store()->tz_min, &y, &mo, &d, &h, &mi);
        lv_label_set_text_fmt(s_file_lbl, "REC-%04d%02d%02d-%02d%02d",
                              y, mo, d, h, mi);
    } else if (s_file_lbl) {
        lv_label_set_text(s_file_lbl, "REC-????");
    }
}

void app_record_enter(void)
{
    s_scr = ui_pixel_screen_create("RECORDING");
    s_bat = app_battery_create(s_scr);

    s_mode_lbl = ui_pixel_label(s_scr, "READY", &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_width(s_mode_lbl, 60);
    lv_obj_set_pos(s_mode_lbl, 170, 30);
    lv_obj_set_style_text_align(s_mode_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *file_panel = ui_pixel_panel_create(s_scr, 10, 62, 220, 42, UI_SURFACE);
    lv_obj_set_style_pad_all(file_panel, 0, 0);
    s_file_lbl = ui_pixel_label(file_panel, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_file_lbl, 10, 2);
    lv_obj_t *codec = ui_pixel_label(file_panel,
                                     "ADPCM / 16K / MONO",
                                     &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_pos(codec, 10, 20);
    lv_obj_t *chevron = ui_pixel_label(file_panel, ">", &lv_font_montserrat_20,
                                       UI_INK);
    lv_obj_set_pos(chevron, 201, 8);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 109, 220, 124, UI_SURFACE);
    lv_obj_set_style_pad_all(panel, 0, 0);

    s_dot = lv_obj_create(panel);
    lv_obj_set_size(s_dot, 8, 8);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_pos(s_dot, 64, 9);
    lv_obj_set_style_opa(s_dot, LV_OPA_TRANSP, 0);

    s_time_lbl = ui_pixel_label(panel, "00:00", &lv_font_montserrat_20, UI_INK);
    lv_obj_set_width(s_time_lbl, 220);
    lv_obj_set_pos(s_time_lbl, 0, 25);
    lv_obj_set_style_text_align(s_time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_state_lbl = ui_pixel_label(panel, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(s_state_lbl, 160);
    lv_obj_set_pos(s_state_lbl, 30, 2);
    lv_obj_set_style_text_align(s_state_lbl, LV_TEXT_ALIGN_CENTER, 0);

    reel_create(panel, 18, 57);
    reel_create(panel, 152, 57);
    lv_obj_t *transfer = ui_pixel_label(panel, ">>>", &lv_font_montserrat_20,
                                        UI_INK);
    lv_obj_set_width(transfer, 70);
    lv_obj_set_pos(transfer, 75, 69);
    lv_obj_set_style_text_align(transfer, LV_TEXT_ALIGN_CENTER, 0);
    shape(panel, 42, 111, 136, 2, UI_INK, 0, 0);
    shape(panel, 37, 107, 10, 10, UI_INK, 0, LV_RADIUS_CIRCLE);
    shape(panel, 173, 107, 10, 10, UI_INK, 0, LV_RADIUS_CIRCLE);

    lv_obj_t *action = ui_pixel_panel_create(s_scr, 10, 238, 220, 44, UI_INK);
    lv_obj_set_style_pad_all(action, 0, 0);
    shape(action, 146, 0, 73, 43, UI_SURFACE, 0, 0);
    shape(action, 145, 0, 1, 43, UI_INK, 0, 0);
    s_action_lbl = ui_pixel_label(action, "OK  START", &lv_font_montserrat_14,
                                  UI_SURFACE);
    lv_obj_set_pos(s_action_lbl, 10, 1);
    s_foot_lbl = ui_pixel_label(action, "", &lv_font_montserrat_14, UI_LINE);
    lv_obj_set_width(s_foot_lbl, 130);
    lv_obj_set_pos(s_foot_lbl, 10, 21);
    lv_obj_t *back = ui_pixel_label(action, "2X BACK", &lv_font_montserrat_14,
                                    UI_INK);
    lv_obj_set_width(back, 73);
    lv_obj_set_pos(back, 146, 11);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);

    ui_pixel_nav_create(s_scr, 0);

    lv_screen_load(s_scr);

    // 启动录音任务与刷新定时器
    s_cmd = REC_CMD_NONE;
    s_session_cmd = REC_CMD_NONE;
    s_recording = false;
    s_samples = 0;
    if (!s_sem) s_sem = xSemaphoreCreateBinary();
    if (!s_task) {
        xTaskCreate(record_task, "app_record", 4096, NULL, 5, &s_task);
    }
    if (!s_timer) s_timer = lv_timer_create(tick, 1000, NULL);

    // 探测 codec 并刷新状态
    s_audio_ok = (bsp_audio_set_format(REC_HZ, 16, 1) == ESP_OK);
    file_label();
    lv_label_set_text(s_state_lbl,
                      !s_audio_ok ? "MIC UNAVAILABLE" :
                      (sync_ble_is_connected() ? "READY" : "NO PHONE"));
    app_record_refresh();
}

void app_record_refresh(void)
{
    if (!s_scr) return;
    lv_label_set_text(s_foot_lbl,
                      sync_ble_is_connected() ? "PHONE READY" : "NO PHONE");
    if (!s_recording) {
        lv_label_set_text(s_state_lbl,
                          !s_audio_ok ? "MIC UNAVAILABLE" :
                          (sync_ble_is_connected() ? "READY" : "NO PHONE"));
    }
    file_label();
}

void app_record_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;
    if (!s_recording) {
        if (!sync_ble_is_connected()) {
            lv_label_set_text(s_state_lbl, "NO PHONE");
            return;
        }
        if (!s_audio_ok) {
            lv_label_set_text(s_state_lbl, "MIC UNAVAILABLE");
            return;
        }
        s_cmd = REC_CMD_START;
        xSemaphoreGive(s_sem);
        lv_label_set_text(s_state_lbl, "STARTING");
    } else {
        s_session_cmd = REC_CMD_STOP;          // 会话循环 ≤30ms 内响应
        lv_label_set_text(s_state_lbl, "FINALIZING");
    }
}

// 退出:录音中先要求会话定稿;任务收 QUIT 后自行退出。不等待任务(它不再碰 UI)。
void app_record_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) {
        app_battery_unregister(s_bat);
        s_bat = NULL;
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_dot = s_time_lbl = s_state_lbl = s_file_lbl = s_foot_lbl = NULL;
        s_mode_lbl = s_action_lbl = NULL;
    }
    if (s_task) {
        if (s_recording) s_session_cmd = REC_CMD_STOP;   // 定稿再退出
        s_cmd = REC_CMD_QUIT;
        xSemaphoreGive(s_sem);
    }
}
