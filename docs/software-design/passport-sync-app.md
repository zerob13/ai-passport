<p align="right">
  <a href="passport-sync-app.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport Sync App — Design

Applies to: `main/app_*.c`, `main/sync_*.c`, `main/adpcm_ima.*`, `main/pager_core.*`, the
`PASSPORT-SYNC` BLE service, and the LVGL screens built from `ui_pixel` theme.

Scope: a companion-device UI for the AI Passport board (ESP32-C3, 240x320 LCD,
3 ADC keys, ES8311 mic/speaker) that syncs with a phone app over BLE. The phone
app is the sync hub: it imports visible instances from the Android system
calendar, owns todo data, archives recordings, and provides the current time.

## 1. Interaction model

Two modes, driven by the three keys (UP / DOWN / OK) through the BSP button
events (`BSP_BTN_CLICK`, `BSP_BTN_DOUBLE`, `BSP_BTN_LONG`). The espressif/button
component resolves single vs double click atomically, so a double click never
also fires a single click.

### Paging mode (default after boot)

| Key | Action |
| --- | --- |
| UP / DOWN click | Flip page (Recording → Schedule → Todo, wraps) |
| OK click | Enter the selected page (switch to in-page mode) |
| OK double | No-op |
| OK long | Play the shutdown chime, turn off the display, and enter deep sleep |

The paging screen shows one page card at a time (title, short hint, page
indicator `N/3`) and the battery level in the sky area.

Normal application startup plays an original short ascending system chime.
Paging-mode shutdown plays an original descending chime before deep sleep.
The dedicated hardware power button is not exposed through the documented BSP,
so a hard power cut cannot reliably trigger firmware audio; power-cycle the
hardware button to start again after software shutdown.

### In-page mode

| Key | Action |
| --- | --- |
| UP / DOWN click | Page-specific navigation (see pages below) |
| OK click | Page-specific primary action |
| OK double | Return to paging mode (global) |
| OK long | Return to paging mode (backup, global) |

Exiting a page stops every task, timer, and callback that can touch that page's
UI before its screen is deleted.

### Pages

1. **Recording** — `app_record.c`
   - OK click toggles recording start / stop.
   - While recording: mic audio (16 kHz, 16-bit, mono) is IMA-ADPCM encoded on
     device (4:1) and streamed to the phone over BLE; the phone stores the file.
   - Screen shows elapsed time, live status (rec / linked / error), and the
     current file label derived from device time.
   - UP / DOWN: no-op in v1 (nothing to navigate inside the page).
   - Leaving the page (OK double / long, or BLE disconnect) while recording
     stops and finalizes the recording first.
   - Requires a live BLE link; if the link is lost mid-recording the device
     stops and shows an error (the phone keeps the partial data up to the loss).

2. **Today's schedule** — `app_schedule.c`
   - Data pushed by the phone (`SCHEDULE_CLEAR` + `SCHEDULE_ADD`, see §3).
   - One event per screen ("card"): time range, title, position `i/N`.
   - UP / DOWN click: previous / next event (wraps).
   - OK click: no-op in v1.
   - Header shows today's date from the phone-synced clock and timezone.

3. **Todo** — `app_todo.c`
   - Data pushed by the phone (`TODO_CLEAR` + `TODO_ADD`, see §3).
   - UP / DOWN click: move selection.
   - OK click: toggle selected item done / not done and echo
     `TODO_TOGGLE` to the phone (last writer wins on the phone side).
   - Shows checkboxes, `X/Y` counters (done / total).

## 2. Time

The device has no RTC. The phone sends POSIX time + timezone offset in `HELLO`
(and may repeat it on reconnect). The device keeps a monotonic clock derived
from `esp_timer`, enough for the schedule page date, recording file timestamps,
and elapsed-time display until the next reboot.

## 3. BLE sync protocol

### Service

| Item | Value |
| --- | --- |
| Service UUID | `61692d70-6173-7370-6f72-742d73796e63` (ASCII `ai-passport-sync`) |
| TX characteristic (device → phone) | `61692d70-6173-7370-6f72-742d73796e64`, Notify |
| RX characteristic (phone → device) | `61692d70-6173-7370-6f72-742d73796e65`, Write / WriteWithoutResponse |
| Device name | `FoloPassport` (single constant, changeable) |

The phone should request an ATT MTU of 512 right after connect; all device
payloads are ≤ 240 bytes so the protocol also works at the Android default
MTU 247. The service is a Level-2 leaf: it has no dependencies on the
mini-program Recovery service (FFF0–FFF4) and does not touch protected
partitions.

### Framing (both directions)

```
byte 0      0xA5 header
byte 1      type
byte 2      length L (payload length, L ≤ 240)
bytes 3..   payload (L bytes)
```

Multi-field values are little-endian. Unknown types are ignored and logged.
Invalid header → link is de-synchronized; the phone reconnects.

### RX: phone → device writes

| Type | Name | Payload |
| --- | --- | --- |
| `0x01` | `HELLO` | `ver u8` (protocol version, currently `1`), `unix_time u32`, `tz_min i16` (minutes east of UTC, e.g. +480) |
| `0x02` | `SCHEDULE_CLEAR` | — |
| `0x03` | `SCHEDULE_ADD` | `id u16`, `start_min u16` (minutes since midnight), `end_min u16`, `title_len u8` (≤ 60), `title utf8` |
| `0x05` | `TODO_CLEAR` | — |
| `0x06` | `TODO_ADD` | `id u16`, `done u8`, `title_len u8` (≤ 60), `title utf8` |

`SCHEDULE_ADD` / `TODO_ADD` insert or replace the item with the same `id`.
Titles are UTF-8; the screen renders them with the embedded CJK font, missing
glyphs fall back to the Latin font. The store keeps only today's schedule and
the full todo list, in RAM (phone re-pushes on every connect; nothing is
persisted to flash).

### TX: device → phone notifications

| Type | Name | Payload |
| --- | --- | --- |
| `0x10` | `AUDIO_START` | `unix_time u32`, `sample_rate u16` (16000), `codec u8` (1 = IMA ADPCM 4-bit), `channels u8` (1) |
| `0x11` | `AUDIO_DATA` | `seq u16` (incrementing, wraps), `data` (ADPCM bytes, ≤ 238) |
| `0x12` | `AUDIO_END` | `duration_ms u32`, `pcm_samples u32`, `dropped_bytes u32` |
| `0x20` | `TODO_TOGGLE` | `id u16`, `done u8` |
| `0x30` | `STATUS` | `soc u8` (0-100, `0xFF` unknown), `flags u8` (bit0 = recording, bit1 = charging), `battery_mv u16` (0 = unknown) |

### Sync flows

- **Connect**: phone connects → requests MTU → subscribes to TX notify →
  sends `HELLO` → `SCHEDULE_CLEAR` + `SCHEDULE_ADD`×N → `TODO_CLEAR` +
  `TODO_ADD`×N. The device replies `STATUS` when the link is established.
- **Recording**: OK starts → `AUDIO_START` → live `AUDIO_DATA` stream
  (approximately one notification every 60 ms) → OK stops or page exit →
  `AUDIO_END`. The phone decodes each `AUDIO_DATA` payload and finalizes a
  standard 16 kHz, mono, 16-bit PCM WAV file (for example,
  `REC-YYYYMMDD-HHMMSS.wav`) in the public `Music/AI Passport` directory. A
  dropped link mid-recording has no `AUDIO_END`, so the pending partial file is
  discarded.
- **Todo toggle**: device sends `TODO_TOGGLE`; the phone applies it (last
  writer wins). If the phone toggles from its own UI, it sends `TODO_ADD` with
  the new `done` state.
- **Status**: `STATUS` is sent on connect and whenever battery or recording
  state changes; the phone may use it for its own UI.

### Flow control and reliability

The ADPCM stream is ~8 KB/s (≈ 17 notifications/s of 240 B). This is far below
what an MTU-247+ link sustains, so no credit mechanism is needed. `seq` lets
the phone detect gaps. The recording task paces itself through its BLE queue;
if the queue is ever full for more than one chunk, chunks are dropped and
counted into `dropped_bytes` of `AUDIO_END` instead of stalling the mic.

## 4. Audio chain

```
ES8311 mic (PGA +30 dB) → I2S 16 kHz / 16-bit / mono
  → IMA ADPCM 4-bit encoder (pure C, host-tested) → BLE notify payloads
```

IMA ADPCM uses 4-bit nibbles (codec id 1): 16 kHz → 8 KB/s. The encoder is
stateless per chunk with predictor carry-over across chunks, so the stream can
be decoded by the phone with any standard IMA decoder (index/step table reset
at `AUDIO_START`).

## 5. Failure behavior

| Failure | Behavior |
| --- | --- |
| Display / LVGL init fails | Boot log error, system stops (no usable UI) |
| Audio init fails | Recording page disabled with error text |
| BLE init fails | All pages work; recording shows "no link", sync pages show "waiting for phone" |
| No phone data yet | Schedule/todo pages show empty-state hints |
| BLE disconnect while recording | Recording finalizes and stops; error shown |
| Calendar permission denied | Recording and todo remain available; system-calendar import stays disabled |
| Battery read fails | Battery indicator hidden (graceful) |

## 6. Host-testable cores

Pure C, no ESP-IDF/LVGL includes, covered by host tests (`tests/`):

- `pager_core.*` — paging/in-page state machine (input events → actions).
- `app_chime.*` — deterministic startup/shutdown PCM synthesis.
- `adpcm_ima.*` — IMA ADPCM encoder.
- `sync_proto.*` — framing, RX message apply (schedule/todo store), TX message
  build, audio-session lifecycle (idle → streaming → ended).

## 7. Android app integration checklist

1. Scan for `FoloPassport`, connect, `requestMtu(512)`.
2. Discover the service `61692d70-6173-7370-6f72-742d73796e63`; enable
   notifications on TX; write to RX.
3. Request `READ_CALENDAR` only when the user chooses calendar import. Query
   visible `CalendarContract.Instances` off the main thread for the selected
   past/future day range, persist the imported range, and do not edit the
   source calendars.
4. On every connect: `HELLO` → today's imported schedule (up to 32 items) →
   todo push (order shown above).
5. Recording: on `AUDIO_START` open a file and show the live receiver; append
   each `AUDIO_DATA` payload while updating elapsed time and received bytes; on
   `AUDIO_END` finalize and refresh the recording list. The list provides
   playback and deletion. Filename comes from device `unix_time`.
6. On `TODO_TOGGLE`: mark the item done/undone in the app.
7. Show `STATUS` battery/recording state.

Limitations: the app may retain a multi-day imported calendar range, but the v1
device protocol keeps only today's first 32 events in RAM and re-pushes them on
connect. UI text without a glyph in the CJK subset renders as a placeholder
box; UP/DOWN on the recording page is unused in v1.

Related: [agent guide](../development/agent-guide.md),
[hardware guide](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md),
[BLE recovery compatibility](../development/ble-recovery-compatibility.md).
