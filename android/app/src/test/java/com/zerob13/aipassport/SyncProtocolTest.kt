// android/app/src/test/java/com/zerob13/aipassport/SyncProtocolTest.kt
// 协议编解码 JVM 测试(与设备端 C 实现对齐)。
package com.zerob13.aipassport

import com.zerob13.aipassport.proto.FrameCodec
import com.zerob13.aipassport.proto.RxMessages
import com.zerob13.aipassport.proto.ScheduleItem
import com.zerob13.aipassport.proto.SyncProtocol
import com.zerob13.aipassport.proto.TodoItem
import com.zerob13.aipassport.proto.TxMessages
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SyncProtocolTest {

    @Test
    fun passportAdvertisementMatchesNameOrService() {
        assertTrue(SyncProtocol.matchesAdvertisement("FoloPassport", null, emptyList()))
        assertTrue(SyncProtocol.matchesAdvertisement(null, "FoloPassport", emptyList()))
        assertTrue(
            SyncProtocol.matchesAdvertisement(
                null,
                null,
                listOf(SyncProtocol.SERVICE_UUID),
            )
        )
        assertFalse(
            SyncProtocol.matchesAdvertisement(
                "OtherDevice",
                null,
                listOf(java.util.UUID.randomUUID()),
            )
        )
    }

    @Test
    fun frameRoundTrip() {
        val payload = byteArrayOf(0x01, 0x02, 0x03, 0x04)
        val frame = FrameCodec.encode(SyncProtocol.RX_HELLO, payload)!!
        assertEquals(3 + 4, frame.size)
        assertEquals(SyncProtocol.FRAME_HEADER, frame[0])
        assertEquals(SyncProtocol.RX_HELLO.toByte(), frame[1])
        assertEquals(4, frame[2].toInt())

        val decoded = FrameCodec.decode(frame, 0, frame.size)!!
        assertEquals(SyncProtocol.RX_HELLO, decoded.type)
        assertArrayEquals(payload, decoded.payload)
    }

    @Test
    fun frameRejectsBadHeaderAndShort() {
        assertNull(FrameCodec.decode(byteArrayOf(0x00, 0x01, 0x01), 0, 3))
        assertNull(FrameCodec.decode(byteArrayOf(0xA5.toByte(), 0x01), 0, 2))
        // 长度字段超出可用数据
        val bad = byteArrayOf(0xA5.toByte(), 0x01, 0x10, 0x01)
        assertNull(FrameCodec.decode(bad, 0, bad.size))
    }

    @Test
    fun frameRejectsOversizePayload() {
        assertNull(FrameCodec.encode(0x01, ByteArray(SyncProtocol.MAX_PAYLOAD + 1)))
        assertNotNull(FrameCodec.encode(0x01, ByteArray(SyncProtocol.MAX_PAYLOAD)))
    }

    @Test
    fun helloMessage() {
        val frame = RxMessages.hello(1700000000L, 480)!!
        val decoded = FrameCodec.decode(frame, 0, frame.size)!!
        assertEquals(SyncProtocol.RX_HELLO, decoded.type)
        assertEquals(7, decoded.payload.size)
        // ver u8
        assertEquals(SyncProtocol.PROTO_VER.toByte(), decoded.payload[0])
        // unix_time u32 (1700000000 = 0x6553F100 LE)
        assertEquals(0x00, decoded.payload[1].toInt() and 0xFF)
        assertEquals(0xF1, decoded.payload[2].toInt() and 0xFF)
        assertEquals(0x53, decoded.payload[3].toInt() and 0xFF)
        assertEquals(0x65, decoded.payload[4].toInt() and 0xFF)
        // tz_min i16 (+480 = 0x01E0 LE)
        assertEquals(0xE0, decoded.payload[5].toInt() and 0xFF)
        assertEquals(0x01, decoded.payload[6].toInt() and 0xFF)
    }

    @Test
    fun scheduleAddMessage() {
        val item = ScheduleItem(2, 852, 940, "Break")
        val frame = RxMessages.scheduleAdd(item)!!
        val decoded = FrameCodec.decode(frame, 0, frame.size)!!
        assertEquals(SyncProtocol.RX_SCHEDULE_ADD, decoded.type)
        val p = decoded.payload
        assertEquals(7 + 5, p.size)
        assertEquals(2, FrameCodec.getU16(p, 0))
        assertEquals(852, FrameCodec.getU16(p, 2))
        assertEquals(940, FrameCodec.getU16(p, 4))
        assertEquals(5, p[6].toInt())
        assertEquals("Break", p.copyOfRange(7, 12).toString(Charsets.UTF_8))
    }

    @Test
    fun scheduleTitleClampedToMax() {
        val longTitle = "x".repeat(SyncProtocol.MAX_TITLE + 20)
        val frame = RxMessages.scheduleAdd(ScheduleItem(1, 0, 60, longTitle))!!
        val decoded = FrameCodec.decode(frame, 0, frame.size)!!
        val p = decoded.payload
        assertEquals(SyncProtocol.MAX_TITLE.toByte(), p[6])
        assertEquals(7 + SyncProtocol.MAX_TITLE, p.size)
    }

    @Test
    fun scheduleTitleDoesNotSplitUtf8CodePoint() {
        val title = "a".repeat(59) + "中"
        val frame = RxMessages.scheduleAdd(ScheduleItem(1, 0, 60, title))!!
        val payload = FrameCodec.decode(frame, 0, frame.size)!!.payload
        val encodedTitle = payload.copyOfRange(7, payload.size)

        assertEquals(59, payload[6].toInt())
        assertEquals("a".repeat(59), encodedTitle.toString(Charsets.UTF_8))
    }

    @Test
    fun todoAddMessage() {
        val frame = RxMessages.todoAdd(TodoItem(3, true, "Call B"))!!
        val decoded = FrameCodec.decode(frame, 0, frame.size)!!
        assertEquals(SyncProtocol.RX_TODO_ADD, decoded.type)
        val p = decoded.payload
        assertEquals(3, FrameCodec.getU16(p, 0))
        assertEquals(1, p[2].toInt())
        assertEquals(6, p[3].toInt())
        assertEquals("Call B", p.copyOfRange(4, 10).toString(Charsets.UTF_8))
    }

    @Test
    fun audioStartParsing() {
        val payload = ByteArray(8)
        FrameCodec.putU32(payload, 0, 1700000000L)
        FrameCodec.putU16(payload, 4, 16000)
        payload[6] = 1 // IMA ADPCM
        payload[7] = 1 // mono
        val meta = TxMessages.parseAudioStart(payload)!!
        assertEquals(1700000000L, meta.unixTime)
        assertEquals(16000, meta.sampleRate)
        assertEquals(1, meta.codec)
        assertEquals(1, meta.channels)
        assertNull(TxMessages.parseAudioStart(ByteArray(4)))
    }

    @Test
    fun audioDataParsing() {
        val payload = ByteArray(2 + 3)
        FrameCodec.putU16(payload, 0, 0x0102)
        payload[2] = 0x5A
        payload[3] = 0x5B
        payload[4] = 0x5C
        val (seq, data) = TxMessages.parseAudioData(payload)!!
        assertEquals(0x0102, seq)
        assertArrayEquals(byteArrayOf(0x5A, 0x5B, 0x5C), data)
    }

    @Test
    fun audioEndParsing() {
        val payload = ByteArray(12)
        FrameCodec.putU32(payload, 0, 65432L)
        FrameCodec.putU32(payload, 4, 1000000L)
        FrameCodec.putU32(payload, 8, 3L)
        val meta = TxMessages.parseAudioEnd(payload)!!
        assertEquals(65432L, meta.durationMs)
        assertEquals(1000000L, meta.pcmSamples)
        assertEquals(3L, meta.droppedBytes)
    }

    @Test
    fun todoToggleParsing() {
        val payload = ByteArray(3)
        FrameCodec.putU16(payload, 0, 7)
        payload[2] = 1
        val (id, done) = TxMessages.parseTodoToggle(payload)!!
        assertEquals(7, id)
        assertEquals(true, done)
        assertNull(TxMessages.parseTodoToggle(ByteArray(2)))
    }

    @Test
    fun statusParsing() {
        val payload = ByteArray(4)
        payload[0] = 82
        payload[1] = SyncProtocol.FLAG_RECORDING.toByte()
        FrameCodec.putU16(payload, 2, 3900)
        val status = TxMessages.parseStatus(payload)!!
        assertEquals(82, status.soc)
        assertEquals(true, status.recording)
        assertEquals(false, status.charging)
        assertEquals(3900, status.batteryMv)
        // 未知电量
        val unknown = ByteArray(4)
        unknown[0] = 0xFF.toByte()
        assertEquals(-1, TxMessages.parseStatus(unknown)!!.soc)
    }

    @Test
    fun txFrameDispatch() {
        // STATUS 帧走 parse() 分发
        val statusPayload = ByteArray(4)
        statusPayload[0] = 50
        val frame = FrameCodec.encode(SyncProtocol.TX_STATUS, statusPayload)!!
        val parsed = TxMessages.parse(FrameCodec.decode(frame, 0, frame.size)!!)
        assertEquals(50, (parsed as com.zerob13.aipassport.proto.DeviceStatus).soc)

        // 未知类型 → null
        val unknownFrame = FrameCodec.encode(0x7F, byteArrayOf(1))!!
        assertNull(TxMessages.parse(FrameCodec.decode(unknownFrame, 0, unknownFrame.size)!!))
    }
}
