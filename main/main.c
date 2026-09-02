// main/main.c —— DimOS 同步应用入口。
// 初始化外设后直接进入"翻页模式"主界面(录音/日程/任务/音乐),不再有演示菜单。
// 交互模型与 BLE 协议见 docs/software-design/passport-sync-app.md。
#include "app_pager.h"
#include "app_chime.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"       // 错误日志里要打印 BSP_LCD_* 引脚号
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "main";

#define FAP_SCREENSHOT_COMMAND "FAP_SCREENSHOT_V1\n"
#define FAP_SCREENSHOT_HEADER_BYTES 64u
#define FAP_SCREENSHOT_PAYLOAD_BYTES \
    ((size_t)BSP_LCD_W * BSP_LCD_H * sizeof(uint16_t))

// Render the active screen into a temporary RGB565 buffer and send one
// protocol response. The buffer exists only while a creator requests a capture.
static bool send_screenshot(void)
{
    uint8_t *packet = malloc(FAP_SCREENSHOT_HEADER_BYTES +
                             FAP_SCREENSHOT_PAYLOAD_BYTES);
    if (!packet) {
        ESP_LOGE(TAG, "screenshot buffer allocation failed");
        return false;
    }

    uint8_t *pixels = packet + FAP_SCREENSHOT_HEADER_BYTES;
    lv_draw_buf_t draw_buf;
    lv_result_t result = LV_RESULT_INVALID;
    if (bsp_lvgl_lock(5000)) {
        result = lv_draw_buf_init(&draw_buf, BSP_LCD_W, BSP_LCD_H,
                                  LV_COLOR_FORMAT_RGB565,
                                  BSP_LCD_W * sizeof(uint16_t), pixels,
                                  FAP_SCREENSHOT_PAYLOAD_BYTES);
        if (result == LV_RESULT_OK) {
            result = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                  LV_COLOR_FORMAT_RGB565,
                                                  &draw_buf);
        }
        bsp_lvgl_unlock();
    }

    if (result != LV_RESULT_OK || draw_buf.data != pixels ||
        draw_buf.header.w != BSP_LCD_W || draw_buf.header.h != BSP_LCD_H ||
        draw_buf.header.stride != BSP_LCD_W * sizeof(uint16_t)) {
        ESP_LOGE(TAG, "screen snapshot failed");
        free(packet);
        return false;
    }

    char header[FAP_SCREENSHOT_HEADER_BYTES];
    int header_len = snprintf(header, sizeof(header),
                              "FAP_SCREENSHOT_V1 %u %u RGB565LE %u\n",
                              (unsigned)BSP_LCD_W, (unsigned)BSP_LCD_H,
                              (unsigned)FAP_SCREENSHOT_PAYLOAD_BYTES);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        free(packet);
        return false;
    }

    uint8_t *response = pixels - header_len;
    memcpy(response, header, (size_t)header_len);
    size_t response_len = (size_t)header_len + FAP_SCREENSHOT_PAYLOAD_BYTES;

    // Logs share this USB stream; silence them only while the binary frame is
    // in flight so no unrelated text can corrupt the declared payload.
    esp_log_level_t previous_log_level = esp_log_get_level_master();
    esp_log_set_level_master(ESP_LOG_NONE);
    int sent = usb_serial_jtag_write_bytes(response, response_len,
                                           pdMS_TO_TICKS(10000));
    esp_err_t flushed = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(10000));
    esp_log_set_level_master(previous_log_level);
    free(packet);

    if (sent != (int)response_len || flushed != ESP_OK) {
        ESP_LOGE(TAG, "screen response failed: sent=%d/%u flush=%s", sent,
                 (unsigned)response_len, esp_err_to_name(flushed));
        return false;
    }
    return true;
}

// Match the publisher command byte-for-byte. Other serial input is ignored.
static void screenshot_task(void *arg)
{
    (void)arg;
    static const uint8_t command[] = FAP_SCREENSHOT_COMMAND;
    size_t matched = 0;

    while (true) {
        uint8_t byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY) != 1) continue;
        matched = byte == command[matched] ? matched + 1
                                           : (byte == command[0] ? 1 : 0);
        if (matched == sizeof(command) - 1) {
            matched = 0;
            send_screenshot();
        }
    }
}

static void screenshot_start(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
            ESP_LOGE(TAG, "USB serial driver initialization failed");
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(screenshot_task, "fap_screenshot", 4096, NULL, 4, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "screenshot task creation failed");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DimOS sync application starting");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是所有页面的载体,失败就直接退出。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 各外设单项失败不阻塞:录音页会显示"麦克风不可用",电量指示自动隐藏。
    if (bsp_button_init(app_key_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败,设备将无法操作");
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败,电量指示隐藏");
    }
    bool audio_ready = bsp_audio_init() == ESP_OK;
    if (!audio_ready) {
        ESP_LOGW(TAG, "音频初始化失败,录音页将禁用");
    }

    if (bsp_lvgl_lock(1000)) {
        app_pager_start();        // 启动 BLE 同步服务 + 翻页界面
        bsp_lvgl_unlock();
    }

    if (audio_ready && !app_chime_play(APP_CHIME_STARTUP)) {
        ESP_LOGW(TAG, "startup chime playback failed");
    }

    ESP_LOGI(TAG, "就绪:Battery=%d Audio=%d",
             (int)(bsp_battery_soc() >= 0),
             (int)audio_ready);
    screenshot_start();
}
