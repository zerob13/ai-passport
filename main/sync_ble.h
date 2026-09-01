// main/sync_ble.h —— PASSPORT-SYNC BLE 服务:广播 + GATT TX(notify)/RX(write)。
// 协议定义见 docs/software-design/passport-sync-app.md;UUID 与帧格式见 sync_proto.h。
#pragma once

#include "esp_err.h"
#include "sync_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// BLE 生命周期事件(回调运行在 NimBLE host 任务上下文,勿阻塞)。
typedef enum {
    SYNC_BLE_CONNECTED = 0,    // 已建链(可能尚未完成 MTU/订阅)
    SYNC_BLE_DISCONNECTED,     // 断链
    SYNC_BLE_DATA,             // 收到并处理了一帧 RX 数据(store 已更新)
    SYNC_BLE_MEDIA,            // Now Playing 状态已更新(封面分片完成后才触发)
} sync_ble_evt_t;

typedef void (*sync_ble_cb_t)(sync_ble_evt_t ev);

// 启动广播并注册 GATT 服务(幂等;只启动不停止,应用生命周期内常开)。
esp_err_t sync_ble_start(sync_ble_cb_t cb);

// 当前是否有手机连接(volatile 快照,随时可读)。
bool sync_ble_is_connected(void);

// 发送一帧(任意任务可调,持续约 17 帧/s 的录音流就是从这里走的)。
// 未连接/未订阅返回 ESP_ERR_INVALID_STATE;链路拥塞丢弃时返回 ESP_FAIL。
esp_err_t sync_ble_send(const uint8_t *frame, size_t len);

// 共享同步存储(协议 RX 应用的对象;页面直接读它渲染)。
sync_store_t *app_store(void);

// 便捷:按 store 当前时间发 STATUS(电量读数尽力而为)。
void sync_ble_send_status_now(void);

// 录音标志(录音页独占维护)。置位变化时会自动补发一条 STATUS。
void sync_ble_set_recording(bool on);

// 当前 Unix 时间 = HELLO 时间 + 已流逝时间(任意任务可调)。未对时返回 false。
bool sync_ble_now(uint32_t *unix_secs);

#ifdef __cplusplus
}
#endif
