// main/main.c —— AI Passport 同步应用入口。
// 初始化外设后直接进入"翻页模式"主界面(录音/当天日程/任务),不再有演示菜单。
// 交互模型与 BLE 协议见 docs/software-design/passport-sync-app.md。
#include "app_pager.h"
#include "app_chime.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"       // 错误日志里要打印 BSP_LCD_* 引脚号
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include <stdbool.h>

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 同步应用启动");
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
}
