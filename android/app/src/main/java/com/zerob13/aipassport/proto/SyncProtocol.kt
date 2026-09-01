// android/.../proto/SyncProtocol.kt —— ai-passport-sync 协议:帧编解码与消息构造。
// 与设备端 main/sync_proto.h 对齐,协议定义见
// docs/software-design/passport-sync-app.md(中英双语)。
package com.zerob13.aipassport.proto

import java.util.UUID

object SyncProtocol {
    // GATT 服务/特征 UUID(小写,与设备端一致)
    val SERVICE_UUID: UUID = UUID.fromString("61692d70-6173-7370-6f72-742d73796e63")
    val TX_UUID: UUID = UUID.fromString("61692d70-6173-7370-6f72-742d73796e64") // 设备→手机, Notify
    val RX_UUID: UUID = UUID.fromString("61692d70-6173-7370-6f72-742d73796e65") // 手机→设备, Write
    const val DEVICE_NAME = "FoloPassport"
    const val PREFERRED_MTU = 512

    fun matchesAdvertisement(
        advertisedName: String?,
        cachedName: String?,
        advertisedServices: Collection<UUID>,
    ): Boolean =
        advertisedName == DEVICE_NAME ||
            cachedName == DEVICE_NAME ||
            SERVICE_UUID in advertisedServices

    const val PROTO_VER = 1
    const val MAX_TITLE = 60
    const val MAX_PAYLOAD = 240
    const val AUDIO_DATA_MAX = MAX_PAYLOAD - 2 // seq(2B) 之外的负载上限
    const val CODEC_IMA_ADPCM = 1

    // 帧头
    const val FRAME_HEADER = 0xA5.toByte()

    // RX(手机→设备)消息类型
    const val RX_HELLO = 0x01
    const val RX_SCHEDULE_CLEAR = 0x02
    const val RX_SCHEDULE_ADD = 0x03
    const val RX_TODO_CLEAR = 0x05
    const val RX_TODO_ADD = 0x06

    // TX(设备→手机)消息类型
    const val TX_AUDIO_START = 0x10
    const val TX_AUDIO_DATA = 0x11
    const val TX_AUDIO_END = 0x12
    const val TX_TODO_TOGGLE = 0x20
    const val TX_STATUS = 0x30

    // 状态标志
    const val FLAG_RECORDING = 0x01
    const val FLAG_CHARGING = 0x02
}

/** 一条日程(与设备端 sync_sched_item_t 对齐) */
data class ScheduleItem(
    val id: Int,
    val startMin: Int,
    val endMin: Int,
    val title: String,
)

/** 一条 Todo(与设备端 sync_todo_item_t 对齐) */
data class TodoItem(
    val id: Int,
    var done: Boolean,
    val title: String,
)

/** 设备状态(STATUS 消息) */
data class DeviceStatus(
    val soc: Int,          // -1 = 未知
    val recording: Boolean,
    val charging: Boolean,
    val batteryMv: Int,
)

/** 一段录音的元信息(AUDIO_START / AUDIO_END) */
data class RecordingMeta(
    val unixTime: Long,
    val sampleRate: Int,
    val codec: Int,
    val channels: Int,
    val durationMs: Long,
    val pcmSamples: Long,
    val droppedBytes: Long,
)

/** 帧编解码(纯 Kotlin,可 JVM 测试) */
object FrameCodec {
    class Frame(val type: Int, val payload: ByteArray) {
        val len: Int get() = payload.size
    }

    /** 解析一帧。帧头/长度非法返回 null。 */
    fun decode(data: ByteArray, offset: Int, length: Int): Frame? {
        if (length < 3) return null
        if (data[offset] != SyncProtocol.FRAME_HEADER) return null
        val len = data[offset + 2].toInt() and 0xFF
        if (length < 3 + len) return null
        val type = data[offset + 1].toInt() and 0xFF
        return Frame(type, data.copyOfRange(offset + 3, offset + 3 + len))
    }

    /** 构造一帧。payload 超过上限返回 null。 */
    fun encode(type: Int, payload: ByteArray): ByteArray? {
        if (payload.size > SyncProtocol.MAX_PAYLOAD) return null
        val out = ByteArray(3 + payload.size)
        out[0] = SyncProtocol.FRAME_HEADER
        out[1] = type.toByte()
        out[2] = payload.size.toByte()
        payload.copyInto(out, 3)
        return out
    }

    // ---- 小端读写 ----
    fun putU16(b: ByteArray, off: Int, v: Int) {
        b[off] = (v and 0xFF).toByte()
        b[off + 1] = ((v ushr 8) and 0xFF).toByte()
    }

    fun putU32(b: ByteArray, off: Int, v: Long) {
        for (i in 0 until 4) b[off + i] = ((v ushr (8 * i)) and 0xFF).toByte()
    }

    fun getU16(b: ByteArray, off: Int): Int =
        (b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8)

    fun getU32(b: ByteArray, off: Int): Long {
        var v = 0L
        for (i in 0 until 4) v = v or ((b[off + i].toLong() and 0xFF) shl (8 * i))
        return v
    }

    fun getI16(b: ByteArray, off: Int): Int {
        val u = getU16(b, off)
        return if (u >= 0x8000) u - 0x10000 else u
    }
}

/** 手机→设备 消息构造(纯 Kotlin,可 JVM 测试) */
object RxMessages {
    private fun titleBytes(title: String): ByteArray {
        val bytes = title.encodeToByteArray()
        if (bytes.size <= SyncProtocol.MAX_TITLE) return bytes

        var length = SyncProtocol.MAX_TITLE
        while (length > 0 && (bytes[length].toInt() and 0xC0) == 0x80) length--
        return bytes.copyOf(length)
    }

    /** HELLO: ver u8, unix_time u32, tz_min i16 */
    fun hello(unixTime: Long, tzOffsetMin: Int): ByteArray? {
        val p = ByteArray(7)
        p[0] = SyncProtocol.PROTO_VER.toByte()
        FrameCodec.putU32(p, 1, unixTime)
        FrameCodec.putU16(p, 5, tzOffsetMin)
        return FrameCodec.encode(SyncProtocol.RX_HELLO, p)
    }

    fun scheduleClear(): ByteArray? =
        FrameCodec.encode(SyncProtocol.RX_SCHEDULE_CLEAR, ByteArray(0))

    /** SCHEDULE_ADD: id u16, start u16, end u16, title_len u8, title */
    fun scheduleAdd(item: ScheduleItem): ByteArray? {
        val t = titleBytes(item.title)
        val titleLen = t.size
        val p = ByteArray(7 + titleLen)
        FrameCodec.putU16(p, 0, item.id)
        FrameCodec.putU16(p, 2, item.startMin)
        FrameCodec.putU16(p, 4, item.endMin)
        p[6] = titleLen.toByte()
        t.copyInto(p, 7, 0, titleLen)
        return FrameCodec.encode(SyncProtocol.RX_SCHEDULE_ADD, p)
    }

    fun todoClear(): ByteArray? =
        FrameCodec.encode(SyncProtocol.RX_TODO_CLEAR, ByteArray(0))

    /** TODO_ADD: id u16, done u8, title_len u8, title */
    fun todoAdd(item: TodoItem): ByteArray? {
        val t = titleBytes(item.title)
        val titleLen = t.size
        val p = ByteArray(4 + titleLen)
        FrameCodec.putU16(p, 0, item.id)
        p[2] = if (item.done) 1 else 0
        p[3] = titleLen.toByte()
        t.copyInto(p, 4, 0, titleLen)
        return FrameCodec.encode(SyncProtocol.RX_TODO_ADD, p)
    }
}

/** 设备→手机 消息解析(纯 Kotlin,可 JVM 测试) */
object TxMessages {
    /** 尝试解析一条 TX 帧。返回 null = 帧格式错误或类型未知。 */
    fun parse(frame: FrameCodec.Frame): Any? = when (frame.type) {
        SyncProtocol.TX_AUDIO_START -> parseAudioStart(frame.payload)
        SyncProtocol.TX_AUDIO_END -> parseAudioEnd(frame.payload)
        SyncProtocol.TX_TODO_TOGGLE -> parseTodoToggle(frame.payload)
        SyncProtocol.TX_STATUS -> parseStatus(frame.payload)
        SyncProtocol.TX_AUDIO_DATA -> parseAudioData(frame.payload)
        else -> null
    }

    /** AUDIO_START: unix_time u32, sample_rate u16, codec u8, channels u8 */
    fun parseAudioStart(p: ByteArray): RecordingMeta? {
        if (p.size < 8) return null
        return RecordingMeta(
            unixTime = FrameCodec.getU32(p, 0),
            sampleRate = FrameCodec.getU16(p, 4),
            codec = p[6].toInt() and 0xFF,
            channels = p[7].toInt() and 0xFF,
            durationMs = 0, pcmSamples = 0, droppedBytes = 0,
        )
    }

    /** AUDIO_DATA: seq u16, data */
    fun parseAudioData(p: ByteArray): Pair<Int, ByteArray>? {
        if (p.size < 2) return null
        return FrameCodec.getU16(p, 0) to p.copyOfRange(2, p.size)
    }

    /** AUDIO_END: duration_ms u32, pcm_samples u32, dropped_bytes u32 */
    fun parseAudioEnd(p: ByteArray): RecordingMeta? {
        if (p.size < 12) return null
        return RecordingMeta(
            unixTime = 0, sampleRate = 0, codec = 0, channels = 0,
            durationMs = FrameCodec.getU32(p, 0),
            pcmSamples = FrameCodec.getU32(p, 4),
            droppedBytes = FrameCodec.getU32(p, 8),
        )
    }

    /** TODO_TOGGLE: id u16, done u8 */
    fun parseTodoToggle(p: ByteArray): Pair<Int, Boolean>? {
        if (p.size < 3) return null
        return FrameCodec.getU16(p, 0) to (p[2].toInt() != 0)
    }

    /** STATUS: soc u8, flags u8, battery_mv u16 */
    fun parseStatus(p: ByteArray): DeviceStatus? {
        if (p.size < 4) return null
        val soc = p[0].toInt() and 0xFF
        val flags = p[1].toInt() and 0xFF
        return DeviceStatus(
            soc = if (soc == 0xFF) -1 else soc,
            recording = (flags and SyncProtocol.FLAG_RECORDING) != 0,
            charging = (flags and SyncProtocol.FLAG_CHARGING) != 0,
            batteryMv = FrameCodec.getU16(p, 2),
        )
    }
}
