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
import android.content.Context
import android.os.Handler
import android.os.Looper
import com.zerob13.aipassport.proto.FrameCodec
import com.zerob13.aipassport.proto.RxMessages
import com.zerob13.aipassport.proto.SyncProtocol
import com.zerob13.aipassport.proto.TxMessages
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 会话监听回调(所有回调都投递到主线程)。
 */
interface SyncListener {
    fun onConnectionChanged(connected: Boolean)
    fun onStatus(status: com.zerob13.aipassport.proto.DeviceStatus)
    fun onTodoToggle(id: Int, done: Boolean)
    fun onRecordingStarted()
    fun onRecordingFinished(file: File?, durationMs: Long, droppedBytes: Long)
    fun onError(message: String)
}

/**
 * 一次"正在接收"的录音会话:把 ADPCM 数据追加到文件,收到 AUDIO_END 后定稿。
 */
class RecordingSession(file: File) {
    val output = FileOutputStream(file)
    var durationMs = 0L
    var droppedBytes = 0L
    private var closed = AtomicBoolean(false)

    fun write(data: ByteArray) {
        if (!closed.get()) output.write(data)
    }

    fun close() {
        if (closed.compareAndSet(false, true)) {
            output.flush()
            output.close()
        }
    }
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
    private var pendingWriteQueue = ArrayDeque<ByteArray>()
    private var writeInFlight = false

    private var recording: RecordingSession? = null
    private var recordingFileName: String? = null

    val isConnected: Boolean get() = gatt != null

    // ---------------- 扫描 ----------------
    fun startScan(onFound: (BluetoothDevice) -> Unit) {
        val adapter = bluetoothAdapter ?: return
        val scanner = adapter.bluetoothLeScanner ?: return
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = result.device.name
                if (name == SyncProtocol.DEVICE_NAME) {
                    scanner.stopScan(this)
                    onFound(result.device)
                }
            }
        }
        scanner.startScan(null, settings, callback)
    }

    // ---------------- 连接 ----------------
    fun connect(device: BluetoothDevice) {
        disconnect()
        gatt = device.connectGatt(context, false, gattCallback)
    }

    fun disconnect() {
        pendingWriteQueue.clear()
        writeInFlight = false
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        txChar = null
        rxChar = null
        listener.onConnectionChanged(false)
    }

    // ---------------- GATT 回调 ----------------
    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    listener.onConnectionChanged(true)
                    g.requestMtu(SyncProtocol.PREFERRED_MTU)
                    g.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (gatt === g) {
                        gatt = null
                        txChar = null
                        rxChar = null
                    }
                    listener.onConnectionChanged(false)
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            // MTU 已协商,开始发现服务(discoverServices 已在连接时发起,这里无需重复)
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
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
            pumpWrites()
        }
    }

    private fun enableTxNotifications(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
        g.setCharacteristicNotification(ch, true)
        val cccd = ch.getDescriptor(UUID_CCCD) ?: return
        cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        g.writeDescriptor(cccd)
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

    // ---------------- TX 帧处理 ----------------
    private fun handleTxFrame(frame: FrameCodec.Frame) {
        when (frame.type) {
            SyncProtocol.TX_AUDIO_START -> {
                val meta = TxMessages.parseAudioStart(frame.payload)
                if (meta != null) {
                    startRecording(meta)
                    listener.onRecordingStarted()
                }
            }
            SyncProtocol.TX_AUDIO_DATA -> {
                val d = TxMessages.parseAudioData(frame.payload)
                if (d != null && recording != null) {
                    recording?.write(d.second)
                }
            }
            SyncProtocol.TX_AUDIO_END -> {
                val meta = TxMessages.parseAudioEnd(frame.payload)
                recording?.durationMs = meta?.durationMs ?: 0
                recording?.droppedBytes = meta?.droppedBytes ?: 0
                finishRecording()
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

    private fun startRecording(meta: com.zerob13.aipassport.proto.RecordingMeta) {
        finishRecording() // 防御:上一条未收尾则先定稿
        val dir = File(context.filesDir, "recordings").apply { mkdirs() }
        val stamp = java.text.SimpleDateFormat(
            "yyyyMMdd-HHmmss", java.util.Locale.US
        ).format(java.util.Date(meta.unixTime * 1000))
        val file = File(dir, "REC-$stamp.adpcm")
        recording = RecordingSession(file)
        recordingFileName = file.name
    }

    private fun finishRecording() {
        val s = recording ?: return
        val file = File(context.filesDir, "recordings").let { dir ->
            File(dir, recordingFileName ?: "recording.adpcm")
        }
        s.close()
        recording = null
        listener.onRecordingFinished(file, s.durationMs, s.droppedBytes)
    }

    companion object {
        private val UUID_CCCD =
            java.util.UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}