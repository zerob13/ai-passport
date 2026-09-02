<p align="right">
  <a href="passport-sync-app.md">English</a> · <strong>简体中文</strong>
</p>

# DimOS 同步应用 —— AI Passport 设计文档

适用范围:`main/app_*.c`、`main/sync_*.c`、`main/adpcm_ima.*`、`main/pager_core.*`、
`PASSPORT-SYNC` BLE 服务,以及基于 `ui_pixel` 主题构建的 LVGL 界面。

目标:为 AI Passport 开发板(ESP32-C3、240x320 彩屏、三键、ES8311 麦克风/喇叭)
开发名为 DimOS 的配套设备端界面,通过 BLE 与手机 App 同步。手机 App 是同步中枢:
它从 Android 系统日历导入可见实例、维护 Todo、归档录音、读取 Android 当前媒体
会话,并向设备提供当前时间和正在播放的音乐。

## 1. 交互模型

两种模式,由三个按键(上/下/确定)的 BSP 事件驱动(`BSP_BTN_CLICK`、
`BSP_BTN_DOUBLE`、`BSP_BTN_LONG`)。espressif/button 组件对单击/双击是原子判定:
一次双击绝不会同时触发单击。

### 翻页模式(开机默认)

| 按键 | 动作 |
| --- | --- |
| 上/下 单击 | 翻页(录音 → 日程 → 任务 → 音乐,循环) |
| 确定 单击 | 进入当前页(切换到页面模式) |
| 确定 双击 | 无操作 |
| 确定 长按 | 播放关机提示音、熄屏并进入深睡 |

翻页界面显示四张紧凑页面卡片,当前卡片反色,页码指示为 `N/4`,顶部显示电量。
所有用户可见的固件品牌文字使用 `DimOS`;BLE 名称 `FoloPassport`、服务 UUID 与
刷机路径保持不变，配套数据协议升级为 v2。

应用正常启动后会播放一段原创的短上行系统提示音。翻页模式的软件关机会先播放
原创下行提示音，再进入深睡。现有 BSP 文档没有向固件暴露独立硬件电源键，因此
硬件直接切电时无法可靠播放固件声音；软件关机后需通过硬件电源键重新上电启动。

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
   - 上/下 单击:无操作(页面内没有可上下切换的内容)。
   - 录音中离开页面(确定 双击/长按,或 BLE 断开):先停止并结束录音,再退出。
   - 依赖实时 BLE 链路;录音中途断链则停止并提示错误(手机保留断点前的部分数据)。

2. **日程列表**(`app_schedule.c`)
   - 数据由手机推送(`SCHEDULE_CLEAR` + `SCHEDULE_ADD`,见第 3 节)。
   - 每页显示四条带日期的日程，包含时间或全天状态与标题。
   - 上/下 单击:上一页 / 下一页，到边界后停止。
   - 确定 单击:无操作。
   - 进入页面时默认定位到“今天第一条日程”所在页；今天无日程时，
     定位到第一条未来日程，若没有未来日程则定位到最后一页。
   - 头部显示今天日期、日程总数与当前/总页数。

3. **任务 Todo**(`app_todo.c`)
   - 数据由手机推送(`TODO_CLEAR` + `TODO_ADD`,见第 3 节)。
   - 上/下 单击:移动选中项。
   - 确定 单击:切换选中项的完成状态,并向手机回传 `TODO_TOGGLE`
     (手机侧“后写者胜”)。
   - 显示勾选框与 `X/Y`(完成数 / 总数)。

4. **音乐**(`app_music.c`)
   - 手机通过 Android `MediaSession` 读取当前播放器;Spotify 可用,遵循系统媒体会话
     的其他播放器也通用,无需 Spotify 登录、SDK 或开发者密钥。
   - 显示播放器、歌曲、歌手、专辑、96×96 封面、播放/暂停状态和实时进度。
   - 手机上切歌后重新传元数据和封面;播放期间每秒校正一次进度,设备在两次校正间
     用单调时钟平滑推进。
   - 本页无播放控制;上/下/确定单击无操作,确定双击或长按返回翻页界面。

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
| `0x01` | `HELLO` | `ver u8`(协议版本,当前 `2`)、`unix_time u32`、`tz_min i16`(UTC 东侧分钟数,如 +480) |
| `0x02` | `SCHEDULE_CLEAR` | — |
| `0x03` | `SCHEDULE_ADD` | `id u16`、`epoch_day i32`(从 1970-01-01 起的本地日期天数)、`start_min u16`、`end_min u16`、`flags u8`(bit0 = 全天)、`title_len u8`(≤ 60)、`title utf8` |
| `0x05` | `TODO_CLEAR` | — |
| `0x06` | `TODO_ADD` | `id u16`、`done u8`、`title_len u8`(≤ 60)、`title utf8` |
| `0x08` | `MEDIA_CLEAR` | — |
| `0x09` | `MEDIA_INFO` | `flags u8`(bit0 = 播放中,bit1 = 有封面)、`duration_ms u32`、`position_ms u32`,随后依次为歌曲/歌手/专辑/播放器四个 `len u8 + utf8`;前三项 ≤ 60 B,播放器 ≤ 24 B |
| `0x0A` | `MEDIA_ART_BEGIN` | `total_bytes u16`(固定 18432) |
| `0x0B` | `MEDIA_ART_DATA` | `offset u16`、按顺序发送的 RGB565 小端字节(≤ 238 B) |
| `0x0C` | `MEDIA_ART_END` | `total_bytes u16`(固定 18432) |
| `0x0D` | `MEDIA_PROGRESS` | `flags u8`(bit0 = 播放中)、`position_ms u32`、`duration_ms u32` |

`SCHEDULE_ADD` / `TODO_ADD` 对相同 `id` 为插入或替换。标题为 UTF-8;屏幕用内置
中文字库渲染,缺字回退到西文字体。协议 v2 直接替换 v1 日程负载，必须使用
配套新 App。手机下发最接近当前日期的最多 40 条导入日程；设备在 RAM 中
保留全部 40 条与完整 Todo 列表，按日期排序并在本地分页，不写 flash。封面固定为 96×96 RGB565,
只有顺序和总长度均正确时才对 UI 可见;不完整封面不会显示。

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
  `N` 最多为 40，日期围绕今天选取。若存在媒体会话,随后发送 `MEDIA_INFO` 和封面;
  否则发送 `MEDIA_CLEAR`。设备建链后
  回复 `STATUS`。
- **录音**:确定 开始 → `AUDIO_START` → 实时 `AUDIO_DATA` 流(约每 60 ms 一条)
  → 确定 停止或退出页面 → `AUDIO_END`。手机实时解码每条 `AUDIO_DATA`，
  收到 `AUDIO_END` 后在公共 `音乐/DimOS` 目录定稿为标准 16 kHz、
  单声道、16-bit PCM WAV 文件（如 `REC-YYYYMMDD-HHMMSS.wav`）。中途断链
  不会收到 `AUDIO_END`，此时丢弃未完成文件。
- **Todo 勾选**:设备发 `TODO_TOGGLE`,手机应用该状态(后写者胜)。
  手机端自己勾选时,发 `TODO_ADD` 携带新 `done` 状态即可。
- **状态**:建链、电池或录音状态变化时发送 `STATUS`,手机可用于自己的界面。
- **正在播放**:切歌 → `MEDIA_INFO` → `MEDIA_ART_BEGIN` →
  `MEDIA_ART_DATA`×78 → `MEDIA_ART_END`;播放期间每秒发送 `MEDIA_PROGRESS`。
  快速切歌会从写队列移除旧媒体分片;进度帧只保留最新一条。

### 流控与可靠性

ADPCM 流约 8 KB/s(≈ 每秒 17 条 240 B 通知),远低于 MTU 247+ 链路的承受能力,
无需信用(credit)机制。`seq` 让手机能发现丢包。录音任务通过 BLE 队列自限速;
若队列持续满,则丢块并把字节数计入 `AUDIO_END.dropped_bytes`,而不是卡住麦克风。
封面每次换歌传 18,432 字节,Android 使用带响应的串行写队列保证分片顺序;设备在
接收中不逐分片刷新 LVGL,只在 `MEDIA_ART_END` 校验通过后显示。

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
| 未授予媒体访问权限 | App 显示授权入口;设备音乐页保持等待状态 |
| 播放器无封面 | 仍同步歌曲信息和进度;设备显示 `NO ART` |
| BLE 断开 | 清除易过期的正在播放状态;日程/Todo 数据保留到下次同步 |
| 录音中 BLE 断链 | 先结束并停止录音,再提示错误 |
| 拒绝日历权限 | 录音和 Todo 仍可使用,系统日历导入不可用 |
| 电量读取失败 | 电量指示隐藏(优雅降级) |

## 6. 可主机测试的核心

纯 C,不包含 ESP-IDF/LVGL 头文件,由主机测试覆盖(`tests/`):

- `pager_core.*` —— 翻页/页面状态机(输入事件 → 动作)。
- `app_chime.*` —— 确定性的开机/关机 PCM 音效合成。
- `adpcm_ima.*` —— IMA ADPCM 编码器。
- `sync_proto.*` —— 帧编解码、RX 消息应用(日程/Todo/媒体存储)、封面分片校验、
  TX 消息构造和录音会话生命周期(idle → 流式 → 结束)。

## 7. Android App 对接清单

1. 扫描 `FoloPassport`,连接,`requestMtu(512)`。
2. 发现服务 `61692d70-6173-7370-6f72-742d73796e63`;订阅 TX 通知;写 RX。
3. 仅在用户选择导入日历时申请 `READ_CALENDAR`;按用户选择的过去/未来天数,
   在非主线程查询可见 `CalendarContract.Instances`,保存导入范围,但不修改源日历。
4. 每次连接按上文顺序推送:`HELLO` → 最接近今天的最多 40 条导入日程
   → Todo。不支持协议 v1 的旧 App。
5. 录音:收到 `AUDIO_START` 打开文件并显示实时接收区;追加每条 `AUDIO_DATA`
   时更新时长和接收字节数;收到 `AUDIO_END` 后定稿并刷新录音列表。列表提供播放
   与删除,文件名用设备 `unix_time` 生成。
6. 收到 `TODO_TOGGLE`:在 App 中标记完成/未完成。
7. 用 `STATUS` 展示电量与录音状态。
8. 引导用户在系统“通知使用权”中启用 DimOS。用 `MediaSessionManager` 获取当前
   `MediaController`,监听元数据和播放状态;将封面居中裁切到 96×96 并转换为
   RGB565 小端格式。该实现首先支持 Spotify,同时适用于公开标准媒体会话的播放器。

限制:App 保留完整导入范围，设备接收最接近今天的 40 条并缓存到下次同步；
字库中没有的字形显示为占位框;录音页和音乐页的上/下键未使用;
Android 伴侣进程被系统终止时,BLE 和媒体桥都需要重新打开 App 后恢复。

相关:[开发指南](../development/agent-guide.zh_CN.md)、
[硬件指南](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md)、
[BLE Recovery 兼容性](../development/ble-recovery-compatibility.zh_CN.md)。
