// main/sync_ble.c —— NimBLE GATT 服务实现(见 sync_ble.h)。
#include "sync_ble.h"

#include "bsp_battery.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "sync_ble";

#define DEVICE_NAME "FoloPassport"

// UUID 字符串(参见设计文档):
//   服务  61692d70-6173-7370-6f72-742d73796e63   ASCII "ai-passport-sync"
//   TX    61692d70-6173-7370-6f72-742d73796e64
//   RX    61692d70-6173-7370-6f72-742d73796e65
// NimBLE 的 BLE_UUID128_INIT 按字节序反转(LSB 在前)。
#define UUID_BYTES(tail) \
    0x61, 0x69, 0x2d, 0x70, 0x61, 0x73, 0x73, 0x70, \
    0x6f, 0x72, 0x74, 0x2d, 0x73, 0x79, 0x6e, (tail)

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(UUID_BYTES(0x63));
static const ble_uuid128_t s_tx_uuid  = BLE_UUID128_INIT(UUID_BYTES(0x64));
static const ble_uuid128_t s_rx_uuid  = BLE_UUID128_INIT(UUID_BYTES(0x65));

static sync_store_t s_store;
static sync_ble_cb_t s_cb;
static volatile bool s_connected;
static volatile bool s_subscribed;
static volatile bool s_recording;        // 录音标志(由录音页置位,用于 STATUS)
static int64_t s_hello_us;               // 收到 HELLO 的 esp_timer 时刻
static uint16_t s_conn_handle;
static uint16_t s_tx_handle;

static int gap_event(struct ble_gap_event *event, void *arg);

// ---- GATT 特征访问 ----

static int rx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 帧最大 SYNC_FRAME_MAX,先搬进栈再校验(协议层对长度有严格检查)。
    uint8_t buf[SYNC_FRAME_MAX];
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

    int handled = sync_proto_rx(&s_store, buf, len);
    if (handled < 0) {
        ESP_LOGW(TAG, "RX 帧校验失败(len=%u):%d", (unsigned)len, handled);
        return 0;                        // 格式错误不回复 ATT 错误,记日志即可
    }
    if (handled == SYNC_RX_HELLO) {
        s_hello_us = esp_timer_get_time();   // 记录对时时刻,供 sync_ble_now()
    }
    if (s_cb) s_cb(SYNC_BLE_DATA);       // store 已更新,UI 可刷新
    return 0;
}

static int tx_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    // TX 是纯 Notify 特征:读/写都不开放(0 = 空读,用于调试预览)。
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = tx_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

// ---- 广播 ----

static void advertise(void)
{
    uint8_t addr_type;
    if (ble_hs_id_infer_auto(0, &addr_type) != 0) return;

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;   // 广播服务 UUID,方便手机过滤
    fields.num_uuids128 = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;          // 可连接
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL) != 0) {
        ESP_LOGE(TAG, "广播启动失败");
    }
}

// ---- GAP 事件 ----

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            s_subscribed = false;
            ESP_LOGI(TAG, "手机已连接");
            if (s_cb) s_cb(SYNC_BLE_CONNECTED);
            ble_gap_adv_stop();            // 连接后停止广播
        } else {
            s_connected = false;
            ESP_LOGW(TAG, "连接失败:%d", event->connect.status);
            advertise();                   // 失败后继续广播
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_subscribed = false;
        ESP_LOGI(TAG, "手机已断开");
        if (s_cb) s_cb(SYNC_BLE_DISCONNECTED);
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        s_subscribed = event->subscribe.cur_notify != 0;
        ESP_LOGI(TAG, "TX notify 订阅 %s", s_subscribed ? "开" : "关");
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU 更新为 %u", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

// ---- NimBLE host 初始化 ----

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE 重置:%d", reason);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_att_set_preferred_mtu(512);                    // 收音流用,手机也会请求 MTU
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool s_started;

esp_err_t sync_ble_start(sync_ble_cb_t cb)
{
    if (s_started) return ESP_OK;
    s_cb = cb;
    sync_store_init(&s_store);

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
        err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS 初始化失败:%s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败:%s", esp_err_to_name(err));
        return err;
    }
    s_started = true;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE 已启动,设备名 %s", DEVICE_NAME);
    return ESP_OK;
}

bool sync_ble_is_connected(void)
{
    return s_connected;
}

esp_err_t sync_ble_send(const uint8_t *frame, size_t len)
{
    if (!s_connected || !s_subscribed || len == 0 || len > SYNC_FRAME_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, len);
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

sync_store_t *app_store(void)
{
    return &s_store;
}

// 录音标志由录音页维护,STATUS 与 TODO_TOGGLE 等消息需要它。
void sync_ble_set_recording(bool on)
{
    s_recording = on;
    sync_ble_send_status_now();
}

void sync_ble_send_status_now(void)
{
    if (!s_connected || !s_subscribed) return;
    uint8_t flags = (s_recording ? SYNC_FLAG_RECORDING : 0);
    int soc = bsp_battery_soc();
    int mv = bsp_battery_mv();
    uint8_t frame[SYNC_FRAME_MAX];
    size_t n = sync_proto_build_status(frame, sizeof(frame),
                                       soc >= 0 ? (uint8_t)soc : 0xFF,
                                       flags,
                                       mv > 0 ? (uint16_t)mv : 0);
    if (n) sync_ble_send(frame, n);
}

bool sync_ble_now(uint32_t *unix_secs)
{
    sync_store_t *st = &s_store;
    if (!st->time_set) return false;
    int64_t elapsed_us = s_hello_us ? esp_timer_get_time() - s_hello_us : 0;
    if (elapsed_us < 0) elapsed_us = 0;
    *unix_secs = st->unix_time + (uint32_t)(elapsed_us / 1000000);
    return true;
}
