// android/.../ble/BleSyncClient.kt —— 连接 FoloPassport 并实现 ai-passport-sync 协议。
// 负责:扫描/连接/MTU/发现服务/订阅 TX/写 RX/录音流接收。
package com.zerob13.aipassport.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.bluetooth.le.BluetoothLeScanner
import android.content.Context
import android.os.Handler
import android.os.Looper
import com.zerob13.aipassport.audio.WavRecording
import com.zerob13.aipassport.proto.FrameCodec
import com.zerob13.aipassport.proto.NowPlaying
import com.zerob13.aipassport.proto.RxMessages
import com.zerob13.aipassport.proto.SyncProtocol
import com.zerob13.aipassport.proto.TxMessages

/**
 * 会话监听回调(所有回调都投递到主线程)。
 */
interface SyncListener {
    fun onConnectionChanged(connected: Boolean)
    fun onStatus(status: com.zerob13.aipassport.proto.DeviceStatus)
    fun onTodoToggle(id: Int, done: Boolean)
    fun onRecordingStarted()
    fun onRecordingProgress(durationMs: Long, receivedBytes: Long)
    fun onRecordingFinished(fileName: String?, durationMs: Long, droppedBytes: Long)
    fun onError(message: String)
}

/**
 * BLE 中心客户端。单实例,由 App 持有。
 */
@SuppressLint("MissingPermission")
class BleSyncClient(private val context: Context, private val listener: SyncListener) {

    private val handler = Handler(Looper.getMainLooper())
    private val bluetoothManager =
        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter: BluetoothAdapter? = bluetoothManager.adapter

    private var gatt: BluetoothGatt? = null
    private var txChar: BluetoothGattCharacteristic? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var ready = false
    private var pendingWriteQueue = ArrayDeque<ByteArray>()
    private var writeInFlight = false

    private var activeScanner: BluetoothLeScanner? = null
    private var scanCallback: ScanCallback? = null
    private val scanTimeout = Runnable {
        stopScan()
        listener.onError("未发现 DimOS，请确认设备已开机并靠近手机")
    }

    private var recording: WavRecording? = null
    private var recordingBytes = 0L
    private var recordingSampleRate = 0

    val isConnected: Boolean get() = ready

    // ---------------- 扫描 ----------------
    fun startScan(onFound: (BluetoothDevice) -> Unit) {
        stopScan()
        val adapter = bluetoothAdapter ?: run {
            listener.onError("蓝牙不可用")
            return
        }
        val scanner = adapter.bluetoothLeScanner ?: run {
            listener.onError("无法启动蓝牙扫描")
            return
        }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val record = result.scanRecord
                val services = record?.serviceUuids?.map { it.uuid }.orEmpty()
                if (SyncProtocol.matchesAdvertisement(
                        record?.deviceName,
                        result.device.name,
                        services,
                    )
                ) {
                    stopScan()
                    onFound(result.device)
                }
            }

            override fun onScanFailed(errorCode: Int) {
                stopScan()
                listener.onError("蓝牙扫描失败 ($errorCode)")
            }
        }
        activeScanner = scanner
        scanCallback = callback
        scanner.startScan(null, settings, callback)
        handler.postDelayed(scanTimeout, SCAN_TIMEOUT_MS)
    }

    private fun stopScan() {
        handler.removeCallbacks(scanTimeout)
        val scanner = activeScanner
        val callback = scanCallback
        activeScanner = null
        scanCallback = null
        if (scanner != null && callback != null) scanner.stopScan(callback)
    }

    // ---------------- 连接 ----------------
    fun connect(device: BluetoothDevice) {
        closeGatt()
        gatt = device.connectGatt(
            context,
            false,
            gattCallback,
            BluetoothDevice.TRANSPORT_LE,
            BluetoothDevice.PHY_LE_1M_MASK,
            handler,
        )
        if (gatt == null) listener.onError("无法连接 DimOS")
    }

    fun disconnect() {
        stopScan()
        val notify = gatt != null || ready
        closeGatt()
        if (notify) listener.onConnectionChanged(false)
    }

    private fun closeGatt() {
        cancelRecording()
        pendingWriteQueue.clear()
        writeInFlight = false
        ready = false
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        txChar = null
        rxChar = null
    }

    // ---------------- GATT 回调 ----------------
    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (gatt !== g) {
                g.close()
                return
            }
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (!g.requestMtu(SyncProtocol.PREFERRED_MTU)) {
                        discoverServices(g)
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    cancelRecording()
                    ready = false
                    gatt = null
                    txChar = null
                    rxChar = null
                    g.close()
                    listener.onConnectionChanged(false)
                }
            }
            if (status != BluetoothGatt.GATT_SUCCESS &&
                newState != BluetoothProfile.STATE_DISCONNECTED
            ) {
                listener.onError("蓝牙连接失败 ($status)")
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            discoverServices(g)
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onError("服务发现失败 ($status)")
                return
            }
            val service: BluetoothGattService? =
                g.getService(SyncProtocol.SERVICE_UUID)
            if (service == null) {
                listener.onError("未发现 ai-passport-sync 服务")
                return
            }
            txChar = service.getCharacteristic(SyncProtocol.TX_UUID)
            rxChar = service.getCharacteristic(SyncProtocol.RX_UUID)
            if (txChar == null || rxChar == null) {
                listener.onError("服务特征不完整")
                return
            }
            enableTxNotifications(g, txChar!!)
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (descriptor.uuid != UUID_CCCD) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onError("通知订阅失败 ($status)")
                return
            }
            ready = true
            listener.onConnectionChanged(true)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            val frame = FrameCodec.decode(
                characteristic.value, 0, characteristic.value.size
            ) ?: return
            handleTxFrame(frame)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            writeInFlight = false
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onError("同步写入失败 ($status)")
            }
            pumpWrites()
        }
    }

    private fun discoverServices(g: BluetoothGatt) {
        if (!g.discoverServices()) listener.onError("无法启动服务发现")
    }

    private fun enableTxNotifications(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
        if (!g.setCharacteristicNotification(ch, true)) {
            listener.onError("无法启用设备通知")
            return
        }
        val cccd = ch.getDescriptor(UUID_CCCD) ?: run {
            listener.onError("设备缺少通知描述符")
            return
        }
        cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        if (!g.writeDescriptor(cccd)) listener.onError("无法订阅设备通知")
    }

    // ---------------- 发送(写 RX) ----------------
    private fun enqueue(frame: ByteArray) {
        pendingWriteQueue.addLast(frame)
        pumpWrites()
    }

    private fun pumpWrites() {
        val g = gatt ?: return
        val ch = rxChar ?: return
        if (writeInFlight || pendingWriteQueue.isEmpty()) return
        val frame = pendingWriteQueue.removeFirst()
        ch.value = frame
        writeInFlight = g.writeCharacteristic(ch)
        if (!writeInFlight) pumpWrites()
    }

    /** 连接后:对时 + 推送日程 + 推送 Todo。 */
    fun pushSnapshot(schedule: List<com.zerob13.aipassport.proto.ScheduleItem>,
                     todos: List<com.zerob13.aipassport.proto.TodoItem>) {
        val tz = java.util.TimeZone.getDefault()
        val offsetMin = tz.getOffset(System.currentTimeMillis()) / 60000
        RxMessages.hello(System.currentTimeMillis() / 1000, offsetMin)?.let(::enqueue)
        RxMessages.scheduleClear()?.let(::enqueue)
        schedule.forEach { RxMessages.scheduleAdd(it)?.let(::enqueue) }
        RxMessages.todoClear()?.let(::enqueue)
        todos.forEach { RxMessages.todoAdd(it)?.let(::enqueue) }
    }

    /** 手机侧勾选变化:回传 TODO_ADD(后写者胜)。 */
    fun sendTodo(item: com.zerob13.aipassport.proto.TodoItem) {
        RxMessages.todoAdd(item)?.let(::enqueue)
    }

    /** Replace queued media state so a rapid track change cannot finish stale artwork. */
    fun sendNowPlaying(item: NowPlaying) {
        if (!ready) return
        pendingWriteQueue.removeAll(::isMediaFrame)
        RxMessages.mediaInfo(item)?.let(::enqueue)
        item.artworkRgb565?.let { art ->
            RxMessages.mediaArtworkFrames(art).forEach(::enqueue)
        }
    }

    fun clearNowPlaying() {
        if (!ready) return
        pendingWriteQueue.removeAll(::isMediaFrame)
        RxMessages.mediaClear()?.let(::enqueue)
    }

    fun sendMediaProgress(positionMs: Long, durationMs: Long, playing: Boolean) {
        if (!ready) return
        pendingWriteQueue.removeAll { frame -> frameType(frame) == SyncProtocol.RX_MEDIA_PROGRESS }
        RxMessages.mediaProgress(positionMs, durationMs, playing)?.let(::enqueue)
    }

    private fun isMediaFrame(frame: ByteArray): Boolean =
        frameType(frame) in SyncProtocol.RX_MEDIA_CLEAR..SyncProtocol.RX_MEDIA_PROGRESS

    private fun frameType(frame: ByteArray): Int =
        if (frame.size > 1) frame[1].toInt() and 0xFF else -1

    // ---------------- TX 帧处理 ----------------
    private fun handleTxFrame(frame: FrameCodec.Frame) {
        when (frame.type) {
            SyncProtocol.TX_AUDIO_START -> {
                val meta = TxMessages.parseAudioStart(frame.payload)
                if (meta != null && startRecording(meta)) {
                    listener.onRecordingStarted()
                }
            }
            SyncProtocol.TX_AUDIO_DATA -> {
                val d = TxMessages.parseAudioData(frame.payload)
                if (d != null && recording != null) {
                    if (recording?.writeAdpcm(d.second) != true) {
                        recording = null
                        recordingBytes = 0
                        recordingSampleRate = 0
                        listener.onError("录音写入失败")
                    } else {
                        recordingBytes += d.second.size
                        val durationMs = if (recordingSampleRate > 0) {
                            recordingBytes * 2 * 1000 / recordingSampleRate
                        } else {
                            0
                        }
                        listener.onRecordingProgress(durationMs, recordingBytes)
                    }
                }
            }
            SyncProtocol.TX_AUDIO_END -> {
                val meta = TxMessages.parseAudioEnd(frame.payload)
                finishRecording(meta)
            }
            SyncProtocol.TX_TODO_TOGGLE -> {
                val t = TxMessages.parseTodoToggle(frame.payload)
                if (t != null) listener.onTodoToggle(t.first, t.second)
            }
            SyncProtocol.TX_STATUS -> {
                val s = TxMessages.parseStatus(frame.payload)
                if (s != null) listener.onStatus(s)
            }
        }
    }

    private fun startRecording(
        meta: com.zerob13.aipassport.proto.RecordingMeta,
    ): Boolean {
        cancelRecording()
        if (meta.codec != SyncProtocol.CODEC_IMA_ADPCM || meta.channels != 1) {
            listener.onError("不支持的录音格式")
            return false
        }
        val stamp = java.text.SimpleDateFormat(
            "yyyyMMdd-HHmmss", java.util.Locale.US
        ).format(java.util.Date(meta.unixTime * 1000))
        recording = WavRecording.create(context, "REC-$stamp.wav", meta.sampleRate)
        if (recording == null) {
            listener.onError("无法创建录音文件")
            return false
        }
        recordingBytes = 0
        recordingSampleRate = meta.sampleRate
        return true
    }

    private fun finishRecording(meta: com.zerob13.aipassport.proto.RecordingMeta?) {
        val s = recording ?: return
        recording = null
        recordingBytes = 0
        recordingSampleRate = 0
        if (!s.finish()) {
            listener.onError("录音保存失败")
            return
        }
        listener.onRecordingFinished(
            s.fileName,
            meta?.durationMs ?: 0,
            meta?.droppedBytes ?: 0,
        )
    }

    private fun cancelRecording() {
        val s = recording ?: return
        recording = null
        recordingBytes = 0
        recordingSampleRate = 0
        s.cancel()
    }

    companion object {
        private const val SCAN_TIMEOUT_MS = 12_000L
        private val UUID_CCCD =
            java.util.UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
