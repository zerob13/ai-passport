// android/.../MainActivity.kt —— AI Passport 同步 App 主界面。
package com.zerob13.aipassport

import android.Manifest
import android.app.AlertDialog
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.NumberPicker
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.zerob13.aipassport.ble.BleSyncClient
import com.zerob13.aipassport.ble.SyncListener
import com.zerob13.aipassport.data.AppRepository
import com.zerob13.aipassport.data.CalendarEventRecord
import com.zerob13.aipassport.data.RecordingRecord
import com.zerob13.aipassport.databinding.ActivityMainBinding
import com.zerob13.aipassport.media.NowPlayingBridge
import com.zerob13.aipassport.proto.DeviceStatus
import com.zerob13.aipassport.proto.NowPlaying
import com.zerob13.aipassport.proto.TodoItem
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity(), SyncListener {

    private lateinit var binding: ActivityMainBinding
    private lateinit var repo: AppRepository
    private lateinit var ble: BleSyncClient
    private lateinit var mediaBridge: NowPlayingBridge
    private var mediaPlayer: MediaPlayer? = null

    private val scheduleAdapter = ScheduleAdapter()
    private val todoAdapter = TodoAdapter()
    private val recordingAdapter = RecordingAdapter()

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (permissionsGranted()) {
                startScanAndConnect()
            } else {
                toast("需要蓝牙权限才能连接设备")
            }
        }

    private val enableBtLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
            val adapter = getSystemService(BluetoothManager::class.java).adapter
            if (adapter?.isEnabled == true && permissionsGranted()) {
                startScanAndConnect()
            }
        }

    private val calendarPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                showCalendarImportDialog()
            } else {
                toast("需要日历读取权限才能导入系统日程")
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        repo = AppRepository(this)
        ble = BleSyncClient(this, this)
        mediaBridge = NowPlayingBridge(
            this,
            ::onNowPlaying,
            ::onMediaProgress,
            ::onMediaClear,
        )

        binding.scheduleList.layoutManager = LinearLayoutManager(this)
        binding.scheduleList.adapter = scheduleAdapter
        binding.todoList.layoutManager = LinearLayoutManager(this)
        binding.todoList.adapter = todoAdapter
        binding.recordingList.layoutManager = LinearLayoutManager(this)
        binding.recordingList.adapter = recordingAdapter

        binding.btnConnect.setOnClickListener {
            if (ble.isConnected) {
                ble.disconnect()
            } else if (!permissionsGranted()) {
                requestPermissions()
            } else {
                startScanAndConnect()
            }
        }
        binding.btnImportSchedule.setOnClickListener { requestCalendarImport() }
        binding.btnAddTodo.setOnClickListener { showAddTodoDialog() }
        binding.btnMediaAccess.setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }

        refreshLists()
    }

    override fun onResume() {
        super.onResume()
        refreshMediaAccess()
    }

    // ---------------- 权限 ----------------
    private fun requiredPermissions(): Array<String> {
        val list = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            list.add(Manifest.permission.BLUETOOTH_SCAN)
            list.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            list.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        return list.toTypedArray()
    }

    private fun permissionsGranted(): Boolean {
        val ctx = this
        return requiredPermissions().all {
            ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requestPermissions() {
        permissionLauncher.launch(requiredPermissions())
    }

    // ---------------- 连接 ----------------
    private fun startScanAndConnect() {
        binding.statusText.text = "正在扫描 DimOS..."
        val adapter = getSystemService(BluetoothManager::class.java).adapter
            ?: run { toast("蓝牙不可用"); return }
        if (adapter.isEnabled) {
            ble.startScan(::onDeviceFound)
        } else {
            enableBtLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        }
    }

    private fun onDeviceFound(device: BluetoothDevice) {
        binding.statusText.text = "连接 ${device.address}..."
        ble.connect(device)
    }

    override fun onDestroy() {
        mediaBridge.stop()
        mediaPlayer?.release()
        mediaPlayer = null
        ble.disconnect()
        super.onDestroy()
    }

    // ---------------- 数据刷新 ----------------
    private fun refreshLists() {
        val schedule = repo.loadCalendarEvents()
        val todos = repo.loadTodos()
        val recordings = repo.loadRecordings()
        scheduleAdapter.submitList(schedule)
        todoAdapter.submitList(todos)
        recordingAdapter.submitList(recordings)
        binding.recordingCount.text = String.format(Locale.US, "%02d", recordings.size)
        binding.recordingEmpty.visibility = if (recordings.isEmpty()) View.VISIBLE else View.GONE
        updateCounters()
    }

    private fun updateCounters() {
        val todos = repo.loadTodos()
        val done = todos.count { it.done }
        val (pastDays, futureDays) = repo.calendarRange()
        binding.scheduleCount.text = String.format(
            Locale.SIMPLIFIED_CHINESE,
            "%02d · -%d/+%d 天",
            repo.loadCalendarEvents().size,
            pastDays,
            futureDays,
        )
        binding.todoCount.text = String.format(
            Locale.SIMPLIFIED_CHINESE,
            "%02d/%02d 完成",
            done,
            todos.size,
        )
    }

    // ---------------- Now Playing bridge ----------------
    private fun refreshMediaAccess() {
        if (!mediaBridge.hasAccess()) {
            mediaBridge.stop()
            binding.mediaSource.text = "MEDIASESSION"
            binding.mediaTitle.text = "需要媒体访问权限"
            binding.mediaMeta.text = "系统会打开通知使用权；App 只读取当前媒体会话"
            binding.mediaProgressBar.visibility = View.GONE
            binding.mediaProgress.visibility = View.GONE
            binding.btnMediaAccess.visibility = View.VISIBLE
            ble.clearNowPlaying()
            return
        }

        binding.btnMediaAccess.visibility = View.GONE
        mediaBridge.start()
    }

    private fun onNowPlaying(item: NowPlaying) {
        binding.mediaSource.text = item.source.ifBlank { "MEDIA" }
        binding.mediaTitle.text = item.title
        binding.mediaMeta.text = if (item.album.isBlank()) {
            item.artist
        } else {
            "${item.artist} · ${item.album}"
        }
        binding.mediaProgressBar.visibility = View.VISIBLE
        binding.mediaProgress.visibility = View.VISIBLE
        updateMediaProgress(item.positionMs, item.durationMs, item.playing)
        ble.sendNowPlaying(item)
    }

    private fun onMediaProgress(positionMs: Long, durationMs: Long, playing: Boolean) {
        updateMediaProgress(positionMs, durationMs, playing)
        ble.sendMediaProgress(positionMs, durationMs, playing)
    }

    private fun updateMediaProgress(positionMs: Long, durationMs: Long, playing: Boolean) {
        binding.mediaProgressBar.progress = if (durationMs > 0) {
            (positionMs.coerceIn(0, durationMs) * 1000 / durationMs).toInt()
        } else {
            0
        }
        binding.mediaProgress.text =
            "${if (playing) "播放中" else "已暂停"}  " +
            "${formatMediaTime(positionMs)} / ${formatMediaTime(durationMs)}"
    }

    private fun onMediaClear() {
        binding.mediaSource.text = "MEDIASESSION"
        binding.mediaTitle.text = "等待播放器"
        binding.mediaMeta.text = "打开 Spotify 或其他播放器并开始播放"
        binding.mediaProgressBar.visibility = View.GONE
        binding.mediaProgress.visibility = View.GONE
        ble.clearNowPlaying()
    }

    private fun formatMediaTime(milliseconds: Long): String {
        val seconds = milliseconds.coerceAtLeast(0) / 1000
        return if (seconds >= 3600) {
            String.format(Locale.US, "%d:%02d:%02d", seconds / 3600,
                seconds / 60 % 60, seconds % 60)
        } else {
            String.format(Locale.US, "%02d:%02d", seconds / 60, seconds % 60)
        }
    }

    // ---------------- Calendar import / Todo ----------------
    private fun requestCalendarImport() {
        if (ContextCompat.checkSelfPermission(
                this,
                Manifest.permission.READ_CALENDAR,
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            showCalendarImportDialog()
        } else {
            calendarPermissionLauncher.launch(Manifest.permission.READ_CALENDAR)
        }
    }

    private fun showCalendarImportDialog() {
        val (savedPastDays, savedFutureDays) = repo.calendarRange()
        val padding = (24 * resources.displayMetrics.density).toInt()
        val form = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, 0, padding, 0)
        }
        fun pickerRow(label: String, initial: Int): Pair<LinearLayout, NumberPicker> {
            val picker = NumberPicker(this).apply {
                minValue = 0
                maxValue = 90
                value = initial.coerceIn(minValue, maxValue)
            }
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = android.view.Gravity.CENTER_VERTICAL
                addView(TextView(this@MainActivity).apply {
                    text = label
                    textSize = 16f
                }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
                addView(picker)
            }
            return row to picker
        }
        val (pastRow, pastPicker) = pickerRow("过去天数", savedPastDays)
        val (futureRow, futurePicker) = pickerRow("未来天数", savedFutureDays)
        form.addView(pastRow)
        form.addView(futureRow)

        AlertDialog.Builder(this)
            .setTitle("导入系统日历")
            .setView(form)
            .setPositiveButton("导入") { _, _ ->
                importCalendar(pastPicker.value, futurePicker.value)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun importCalendar(pastDays: Int, futureDays: Int) {
        binding.btnImportSchedule.isEnabled = false
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching { repo.importCalendar(pastDays, futureDays) }
            }
            binding.btnImportSchedule.isEnabled = true
            result.onSuccess { events ->
                refreshLists()
                pushSnapshotIfConnected()
                toast("已导入 ${events.size} 项，已同步至 DimOS（最多 40 项）")
            }.onFailure {
                toast("系统日历导入失败")
            }
        }
    }

    private fun showAddTodoDialog() {
        val input = android.widget.EditText(this).apply { hint = "任务内容" }
        android.app.AlertDialog.Builder(this)
            .setTitle("新增任务")
            .setView(input)
            .setPositiveButton("添加") { _, _ ->
                val text = input.text.toString().trim()
                if (text.isNotEmpty()) {
                    repo.addTodo(text)
                    pushSnapshotIfConnected()
                    refreshLists()
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun pushSnapshotIfConnected() {
        if (ble.isConnected) ble.pushSnapshot(repo.loadDeviceSchedule(), repo.loadTodos())
    }

    private fun confirmDeleteTodo(item: TodoItem) {
        AlertDialog.Builder(this)
            .setTitle("删除任务")
            .setMessage("确定删除「${item.title}」？")
            .setPositiveButton("删除") { _, _ ->
                repo.deleteTodo(item.id)
                pushSnapshotIfConnected()
                refreshLists()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun playRecording(item: RecordingRecord) {
        mediaPlayer?.release()
        val player = MediaPlayer().apply {
            setAudioAttributes(
                AudioAttributes.Builder()
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .build()
            )
            setOnPreparedListener {
                it.start()
                toast("正在播放 ${item.fileName}")
            }
            setOnCompletionListener {
                it.release()
                if (mediaPlayer === it) mediaPlayer = null
            }
            setOnErrorListener { failedPlayer, _, _ ->
                failedPlayer.release()
                if (mediaPlayer === failedPlayer) mediaPlayer = null
                toast("无法播放录音")
                true
            }
        }
        mediaPlayer = player
        try {
            val uri = item.contentUri
            if (uri != null) {
                player.setDataSource(this, uri)
            } else {
                player.setDataSource(item.filePath ?: error("Missing recording path"))
            }
            player.prepareAsync()
        } catch (_: Exception) {
            player.release()
            mediaPlayer = null
            toast("无法播放录音")
        }
    }

    private fun confirmDeleteRecording(item: RecordingRecord) {
        AlertDialog.Builder(this)
            .setTitle("删除录音")
            .setMessage("确定删除「${item.fileName}」？")
            .setPositiveButton("删除") { _, _ ->
                mediaPlayer?.release()
                mediaPlayer = null
                if (!repo.deleteRecording(item)) toast("录音删除失败")
                refreshLists()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun toast(msg: String) =
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    // ---------------- SyncListener ----------------
    override fun onConnectionChanged(connected: Boolean) {
        binding.btnConnect.text = if (connected) "断开 DimOS" else "连接 DimOS"
        binding.statusText.text = if (connected) "已连接 DimOS" else "未连接"
        binding.llMain.alpha = 1f
        if (!connected) {
            binding.recordingLive.visibility = View.GONE
            binding.recordingEmpty.visibility =
                if (repo.loadRecordings().isEmpty()) View.VISIBLE else View.GONE
        }
        if (connected) {
            ble.pushSnapshot(repo.loadDeviceSchedule(), repo.loadTodos())
            mediaBridge.currentSnapshot()?.let(ble::sendNowPlaying) ?: ble.clearNowPlaying()
        }
    }

    override fun onStatus(status: DeviceStatus) {
        val bat = if (status.soc >= 0) "${status.soc}%" else "--"
        binding.statusText.text =
            "已连接 DimOS · 电量 $bat" +
            (if (status.recording) " · 录音中" else "")
    }

    override fun onTodoToggle(id: Int, done: Boolean) {
        repo.applyTodoToggle(id, done)
        refreshLists()
    }

    override fun onRecordingStarted() {
        binding.statusText.text = "正在接收录音..."
        binding.recordingLive.visibility = View.VISIBLE
        binding.recordingEmpty.visibility = View.GONE
        binding.recordingLiveProgress.text = "00:00 · 0.0 KB"
    }

    override fun onRecordingProgress(durationMs: Long, receivedBytes: Long) {
        val totalSeconds = durationMs / 1000
        binding.recordingLiveProgress.text = String.format(
            "%02d:%02d · %.1f KB",
            totalSeconds / 60,
            totalSeconds % 60,
            receivedBytes / 1024.0,
        )
    }

    override fun onRecordingFinished(fileName: String?, durationMs: Long, droppedBytes: Long) {
        binding.recordingLive.visibility = View.GONE
        refreshLists()
        val dropped = if (droppedBytes > 0) "，丢失 $droppedBytes 字节" else ""
        toast("录音已保存: $fileName (${durationMs / 1000}s$dropped)")
    }

    override fun onError(message: String) {
        binding.recordingLive.visibility = View.GONE
        binding.recordingEmpty.visibility =
            if (repo.loadRecordings().isEmpty()) View.VISIBLE else View.GONE
        binding.statusText.text = message
        toast(message)
    }

    // ---------------- 列表适配器 ----------------
    private class ScheduleVH(v: View) : RecyclerView.ViewHolder(v) {
        val time = v.findViewById<android.widget.TextView>(R.id.item_time)
        val title = v.findViewById<android.widget.TextView>(R.id.item_title)
    }

    private inner class ScheduleAdapter : RecyclerView.Adapter<ScheduleVH>() {
        private val items = mutableListOf<CalendarEventRecord>()
        fun submitList(list: List<CalendarEventRecord>) {
            items.clear(); items.addAll(list); notifyDataSetChanged()
        }
        override fun getItemCount() = items.size
        override fun onCreateViewHolder(p: ViewGroup, vt: Int) =
            ScheduleVH(layoutInflater.inflate(R.layout.item_schedule, p, false))
        override fun onBindViewHolder(h: ScheduleVH, pos: Int) {
            val item = items[pos]
            val zone = if (item.allDay) ZoneOffset.UTC else ZoneId.systemDefault()
            val start = Instant.ofEpochMilli(item.beginMs).atZone(zone)
            val end = Instant.ofEpochMilli(item.endMs).atZone(zone)
            val date = start.format(DATE_FORMAT)
            h.time.text = if (item.allDay) {
                "$date · 全天"
            } else if (start.toLocalDate() == end.toLocalDate()) {
                "$date · ${start.format(TIME_FORMAT)} - ${end.format(TIME_FORMAT)}"
            } else {
                "$date ${start.format(TIME_FORMAT)} - ${end.format(DATE_TIME_FORMAT)}"
            }
            h.title.text = item.title
        }
    }

    private class TodoVH(v: View) : RecyclerView.ViewHolder(v) {
        val check = v.findViewById<android.widget.CheckBox>(R.id.item_check)
        val title = v.findViewById<android.widget.TextView>(R.id.item_title)
        val delete = v.findViewById<Button>(R.id.item_delete)
    }

    private inner class TodoAdapter : RecyclerView.Adapter<TodoVH>() {
        private val items = mutableListOf<TodoItem>()
        fun submitList(list: List<TodoItem>) {
            items.clear(); items.addAll(list); notifyDataSetChanged()
        }
        override fun getItemCount() = items.size
        override fun onCreateViewHolder(p: ViewGroup, vt: Int) =
            TodoVH(layoutInflater.inflate(R.layout.item_todo, p, false))
        override fun onBindViewHolder(h: TodoVH, pos: Int) {
            val item = items[pos]
            h.check.setOnCheckedChangeListener(null)
            h.check.isChecked = item.done
            h.title.text = item.title
            h.title.alpha = if (item.done) 0.5f else 1f
            h.check.setOnCheckedChangeListener { _, checked ->
                if (checked != item.done) {
                    item.done = checked
                    repo.applyTodoToggle(item.id, checked)
                    if (ble.isConnected) ble.sendTodo(item)
                    h.title.alpha = if (checked) 0.5f else 1f
                    updateCounters()
                }
            }
            h.delete.setOnClickListener { confirmDeleteTodo(item) }
        }
    }

    private class RecordingVH(v: View) : RecyclerView.ViewHolder(v) {
        val name = v.findViewById<android.widget.TextView>(R.id.item_name)
        val size = v.findViewById<android.widget.TextView>(R.id.item_size)
        val play = v.findViewById<Button>(R.id.item_play)
        val delete = v.findViewById<Button>(R.id.item_delete)
    }

    private inner class RecordingAdapter : RecyclerView.Adapter<RecordingVH>() {
        private val items = mutableListOf<RecordingRecord>()
        fun submitList(list: List<RecordingRecord>) {
            items.clear(); items.addAll(list); notifyDataSetChanged()
        }
        override fun getItemCount() = items.size
        override fun onCreateViewHolder(p: ViewGroup, vt: Int) =
            RecordingVH(layoutInflater.inflate(R.layout.item_recording, p, false))
        override fun onBindViewHolder(h: RecordingVH, pos: Int) {
            val item = items[pos]
            val totalSeconds = item.durationMs / 1000
            h.name.text = item.fileName
            h.size.text = String.format(
                "%.1f KB · %02d:%02d · %s",
                item.sizeBytes / 1024.0,
                totalSeconds / 60,
                totalSeconds % 60,
                item.locationLabel,
            )
            h.play.setOnClickListener { playRecording(item) }
            h.delete.setOnClickListener { confirmDeleteRecording(item) }
        }
    }

    private companion object {
        val DATE_FORMAT: DateTimeFormatter =
            DateTimeFormatter.ofPattern("MM/dd EEE", Locale.SIMPLIFIED_CHINESE)
        val TIME_FORMAT: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm")
        val DATE_TIME_FORMAT: DateTimeFormatter = DateTimeFormatter.ofPattern("MM/dd HH:mm")
    }
}
