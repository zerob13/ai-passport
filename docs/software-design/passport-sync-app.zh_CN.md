<p align="right">
  <a href="passport-sync-app.md">English</a> · <strong>简体中文</strong>
</p>

# AI Passport 同步应用 —— 设计文档

适用范围:`main/app_*.c`、`main/sync_*.c`、`main/adpcm_ima.*`、`main/pager_core.*`、
`PASSPORT-SYNC` BLE 服务,以及基于 `ui_pixel` 主题构建的 LVGL 界面。

目标:为 AI Passport 开发板(ESP32-C3、240x320 彩屏、三键、ES8311 麦克风/喇叭)
开发配套设备端界面,通过 BLE 与手机 App 同步。手机 App 是同步中枢:它拥有日历与
Todo 数据、归档录音,并向设备提供当前时间。

## 1. 交互模型

两种模式,由三个按键(上/下/确定)的 BSP 事件驱动(`BSP_BTN_CLICK`、
`BSP_BTN_DOUBLE`、`BSP_BTN_LONG`)。espressif/button 组件对单击/双击是原子判定:
一次双击绝不会同时触发单击。

### 翻页模式(开机默认)

| 按键 | 动作 |
| --- | --- |
| 上/下 单击 | 翻页(录音 → 日程 → 任务,循环) |
| 确定 单击 | 进入当前页(切换到页面模式) |
| 确定 双击 / 长按 | 无操作 |

翻页界面每次显示一张页面卡片(标题、提示、页码指示 `N/3`),天空区域显示电量。

### 页面模式

| 按键 | 动作 |
| --- | --- |
| 上/下 单击 | 页面内导航(见各页面) |
| 确定 单击 | 页面主操作 |
| 确定 双击 | 退回翻页模式(全局) |
| 确定 长按 | 退回翻页模式(备用,全局) |

退出页面时必须先停止所有能访问该页 UI 的任务、定时器和回调,再删除屏幕。

### 页面

1. **录音**(`app_record.c`)
   - 确定 单击:开始 / 停止录音(切换)。
   - 录音中:设备把麦克风音频(16 kHz / 16-bit / 单声道)用 IMA ADPCM 4:1 压缩,
     经 BLE 实时推给手机,由手机保存文件。
   - 界面显示已录时长、实时状态(录音中 / 已连接 / 出错)、按设备时间生成的
     文件名标签。
   - 上/下 单击:v1 无操作(页面内没有可上下切换的内容)。
   - 录音中离开页面(确定 双击/长按,或 BLE 断开):先停止并结束录音,再退出。
   - 依赖实时 BLE 链路;录音中途断链则停止并提示错误(手机保留断点前的部分数据)。

2. **当天日程**(`app_schedule.c`)
   - 数据由手机推送(`SCHEDULE_CLEAR` + `SCHEDULE_ADD`,见第 3 节)。
   - 一条日程一屏(“卡片”):时间段、标题、位置 `i/N`。
   - 上/下 单击:上一条 / 下一条(循环)。
   - 确定 单击:v1 无操作。
   - 头部显示手机同步的时钟与时区换算出的今天日期。

3. **任务 Todo**(`app_todo.c`)
   - 数据由手机推送(`TODO_CLEAR` + `TODO_ADD`,见第 3 节)。
   - 上/下 单击:移动选中项。
   - 确定 单击:切换选中项的完成状态,并向手机回传 `TODO_TOGGLE`
     (手机侧“后写者胜”)。
   - 显示勾选框与 `X/Y`(完成数 / 总数)。

## 2. 时间

设备没有 RTC。手机在 `HELLO` 时下发 POSIX 时间与时区偏移(重连可再发)。
设备用 `esp_timer` 维护单调时钟,足以支撑日程页日期、录音文件时间戳和
计时显示(直到下次重启)。

## 3. BLE 同步协议

### 服务

| 项目 | 值 |
| --- | --- |
| 服务 UUID | `61692d70-6173-7370-6f72-742d73796e63`(ASCII `ai-passport-sync`) |
| TX 特征(设备 → 手机) | `61692d70-6173-7370-6f72-742d73796e64`,Notify |
| RX 特征(手机 → 设备) | `61692d70-6173-7370-6f72-742d73796e65`,Write / WriteWithoutResponse |
| 设备名 | `FoloPassport`(单一常量,可改) |

手机连接后应立即请求 ATT MTU 512;设备所有消息 ≤ 240 字节,因此 Android
默认 MTU 247 下协议同样可用。该服务是独立叶子服务,不依赖小程序 Recovery
服务(FFF0–FFF4),也不触碰受保护分区。

### 帧格式(双向)

```
byte 0      0xA5 帧头
byte 1      type 类型
byte 2      length L(负载长度,L ≤ 240)
bytes 3..   负载(L 字节)
```

多字节字段小端序。未知类型忽略并记日志;帧头错误视为链路失步,手机重连即可。

### RX:手机 → 设备(写入)

| 类型 | 名称 | 负载 |
| --- | --- | --- |
| `0x01` | `HELLO` | `ver u8`(协议版本,当前 `1`)、`unix_time u32`、`tz_min i16`(UTC 东侧分钟数,如 +480) |
| `0x02` | `SCHEDULE_CLEAR` | — |
| `0x03` | `SCHEDULE_ADD` | `id u16`、`start_min u16`(当日零点起分钟数)、`end_min u16`、`title_len u8`(≤ 60)、`title utf8` |
| `0x05` | `TODO_CLEAR` | — |
| `0x06` | `TODO_ADD` | `id u16`、`done u8`、`title_len u8`(≤ 60)、`title utf8` |

`SCHEDULE_ADD` / `TODO_ADD` 对相同 `id` 为插入或替换。标题为 UTF-8;屏幕用内置
中文字库渲染,缺字回退到西文字体。存储只保留“当天日程”与完整 Todo 列表,
存放在 RAM(手机每次连接都重新推送,不写 flash)。

### TX:设备 → 手机(通知)

| 类型 | 名称 | 负载 |
| --- | --- | --- |
| `0x10` | `AUDIO_START` | `unix_time u32`、`sample_rate u16`(16000)、`codec u8`(1 = IMA ADPCM 4-bit)、`channels u8`(1) |
| `0x11` | `AUDIO_DATA` | `seq u16`(自增回绕)、`data`(ADPCM 字节,≤ 238) |
| `0x12` | `AUDIO_END` | `duration_ms u32`、`pcm_samples u32`、`dropped_bytes u32` |
| `0x20` | `TODO_TOGGLE` | `id u16`、`done u8` |
| `0x30` | `STATUS` | `soc u8`(0-100,`0xFF` 未知)、`flags u8`(bit0 = 录音中,bit1 = 充电中)、`battery_mv u16`(0 = 未知) |

### 同步流程

- **连接**:手机连接 → 请求 MTU → 订阅 TX 通知 → 发 `HELLO` →
  `SCHEDULE_CLEAR` + `SCHEDULE_ADD`×N → `TODO_CLEAR` + `TODO_ADD`×N。
  设备建链后回复 `STATUS`。
- **录音**:确定 开始 → `AUDIO_START` → 实时 `AUDIO_DATA` 流(约每 60 ms 一条)
  → 确定 停止或退出页面 → `AUDIO_END`。手机把每条 `AUDIO_DATA` 负载追加到
  当前文件;收到 `AUDIO_END` 后定稿文件(如 `REC-YYYYMMDD-HHMMSS.adpcm`),
  再按需解码为 WAV(16 kHz 单声道 16-bit)。中途断链则没有 `AUDIO_END`,
  是否保留部分文件由手机决定。
- **Todo 勾选**:设备发 `TODO_TOGGLE`,手机应用该状态(后写者胜)。
  手机端自己勾选时,发 `TODO_ADD` 携带新 `done` 状态即可。
- **状态**:建链、电池或录音状态变化时发送 `STATUS`,手机可用于自己的界面。

### 流控与可靠性

ADPCM 流约 8 KB/s(≈ 每秒 17 条 240 B 通知),远低于 MTU 247+ 链路的承受能力,
无需信用(credit)机制。`seq` 让手机能发现丢包。录音任务通过 BLE 队列自限速;
若队列持续满,则丢块并把字节数计入 `AUDIO_END.dropped_bytes`,而不是卡住麦克风。

## 4. 音频链路

```
ES8311 麦克风(PGA +30 dB)→ I2S 16 kHz / 16-bit / 单声道
  → IMA ADPCM 4-bit 编码(纯 C,已主机测试)→ BLE 通知负载
```

IMA ADPCM 每采样 4 bit(codec id 1):16 kHz → 8 KB/s。编码器按块处理,
预测器与步长跨块延续(块间携带状态),手机端可用任意标准 IMA 解码器还原
(`AUDIO_START` 时重置步长表)。

## 5. 故障行为

| 故障 | 行为 |
| --- | --- |
| 显示 / LVGL 初始化失败 | 启动日志报错并停止(无可用 UI) |
| 音频初始化失败 | 录音页禁用并显示错误 |
| BLE 初始化失败 | 所有页面可用;录音页显示“无连接”,同步页显示“等待手机” |
| 尚无手机数据 | 日程/Todo 页显示空态提示 |
| 录音中 BLE 断链 | 先结束并停止录音,再提示错误 |
| 电量读取失败 | 电量指示隐藏(优雅降级) |

## 6. 可主机测试的核心

纯 C,不包含 ESP-IDF/LVGL 头文件,由主机测试覆盖(`tests/`):

- `pager_core.*` —— 翻页/页面状态机(输入事件 → 动作)。
- `adpcm_ima.*` —— IMA ADPCM 编码器。
- `sync_proto.*` —— 帧编解码、RX 消息应用(日程/Todo 存储)、TX 消息构造、
  录音会话生命周期(idle → 流式 → 结束)。

## 7. Android App 对接清单

1. 扫描 `FoloPassport`,连接,`requestMtu(512)`。
2. 发现服务 `61692d70-6173-7370-6f72-742d73796e63`;订阅 TX 通知;写 RX。
3. 每次连接按上文顺序推送:`HELLO` → 日程 → Todo。
4. 录音:收到 `AUDIO_START` 打开文件;追加每条 `AUDIO_DATA` 负载;
   收到 `AUDIO_END` 定稿。文件名用设备 `unix_time` 生成。
5. 收到 `TODO_TOGGLE`:在 App 中标记完成/未完成。
6. 可选:用 `STATUS` 展示电量与录音状态。

限制:数据仅存 RAM(连接时重推);字库中没有的字形显示为占位框;
录音页的上/下键 v1 未使用。

相关:[开发指南](../development/agent-guide.zh_CN.md)、
[硬件指南](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)、
[BLE Recovery 兼容性](../development/ble-recovery-compatibility.zh_CN.md)。