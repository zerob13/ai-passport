// android/.../MainActivity.kt —— AI Passport 同步 App 主界面。
package com.zerob13.aipassport

import android.Manifest
import android.app.AlertDialog
import android.app.TimePickerDialog
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.zerob13.aipassport.ble.BleSyncClient
import com.zerob13.aipassport.ble.SyncListener
import com.zerob13.aipassport.data.AppRepository
import com.zerob13.aipassport.data.RecordingRecord
import com.zerob13.aipassport.databinding.ActivityMainBinding
import com.zerob13.aipassport.proto.DeviceStatus
import com.zerob13.aipassport.proto.ScheduleItem
import com.zerob13.aipassport.proto.TodoItem
import java.util.Calendar

class MainActivity : AppCompatActivity(), SyncListener {

    private lateinit var binding: ActivityMainBinding
    private lateinit var repo: AppRepository
    private lateinit var ble: BleSyncClient
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        repo = AppRepository(this)
        ble = BleSyncClient(this, this)

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
        binding.btnAddSchedule.setOnClickListener { showAddScheduleDialog() }
        binding.btnAddTodo.setOnClickListener { showAddTodoDialog() }

        refreshLists()
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
        binding.statusText.text = "正在扫描 FoloPassport..."
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
        mediaPlayer?.release()
        mediaPlayer = null
        ble.disconnect()
        super.onDestroy()
    }

    // ---------------- 数据刷新 ----------------
    private fun refreshLists() {
        scheduleAdapter.submitList(repo.loadSchedule())
        todoAdapter.submitList(repo.loadTodos())
        recordingAdapter.submitList(repo.loadRecordings())
        updateCounters()
    }

    private fun updateCounters() {
        val todos = repo.loadTodos()
        val done = todos.count { it.done }
        binding.scheduleCount.text = "${repo.loadSchedule().size} 项"
        binding.todoCount.text = "$done / ${todos.size} 完成"
    }

    // ---------------- 新增日程 / Todo ----------------
    private fun showAddScheduleDialog() {
        val now = Calendar.getInstance()
        var startMin = now.get(Calendar.HOUR_OF_DAY) * 60 + now.get(Calendar.MINUTE)
        var endMin = (startMin + 60).coerceAtMost(23 * 60 + 59)
        if (startMin == endMin) {
            startMin = 22 * 60
            endMin = 23 * 60
        }

        val padding = (24 * resources.displayMetrics.density).toInt()
        val form = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, 0, padding, 0)
        }
        val startButton = Button(this)
        val endButton = Button(this)
        val titleInput = EditText(this).apply {
            hint = "标题"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            maxLines = 2
        }

        fun refreshTimeButtons() {
            startButton.text = "开始时间  ${formatMinutes(startMin)}"
            endButton.text = "结束时间  ${formatMinutes(endMin)}"
        }
        startButton.setOnClickListener {
            showTimePicker(startMin) { selected ->
                startMin = selected
                if (endMin <= startMin) endMin = (startMin + 60).coerceAtMost(23 * 60 + 59)
                refreshTimeButtons()
            }
        }
        endButton.setOnClickListener {
            showTimePicker(endMin) { selected ->
                endMin = selected
                refreshTimeButtons()
            }
        }
        refreshTimeButtons()
        form.addView(startButton)
        form.addView(endButton)
        form.addView(titleInput)

        val dialog = AlertDialog.Builder(this)
            .setTitle("新增日程")
            .setView(form)
            .setPositiveButton("保存", null)
            .setNegativeButton("取消", null)
            .create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val title = titleInput.text.toString().trim()
                when {
                    title.isEmpty() -> toast("请输入日程标题")
                    endMin <= startMin -> toast("结束时间必须晚于开始时间")
                    else -> {
                        repo.addSchedule(startMin, endMin, title)
                        pushSnapshotIfConnected()
                        refreshLists()
                        dialog.dismiss()
                    }
                }
            }
        }
        dialog.show()
    }

    private fun showTimePicker(initialMin: Int, onSelected: (Int) -> Unit) {
        TimePickerDialog(
            this,
            { _, hour, minute -> onSelected(hour * 60 + minute) },
            initialMin / 60,
            initialMin % 60,
            true,
        ).show()
    }

    private fun formatMinutes(minutes: Int): String =
        String.format("%02d:%02d", minutes / 60, minutes % 60)

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
        if (ble.isConnected) ble.pushSnapshot(repo.loadSchedule(), repo.loadTodos())
    }

    private fun confirmDeleteSchedule(item: ScheduleItem) {
        AlertDialog.Builder(this)
            .setTitle("删除日程")
            .setMessage("确定删除「${item.title}」？")
            .setPositiveButton("删除") { _, _ ->
                repo.deleteSchedule(item.id)
                pushSnapshotIfConnected()
                refreshLists()
            }
            .setNegativeButton("取消", null)
            .show()
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
        binding.btnConnect.text = if (connected) "断开" else "连接设备"
        binding.statusText.text = if (connected) "已连接 FoloPassport" else "未连接"
        binding.llMain.alpha = 1f
        if (connected) {
            ble.pushSnapshot(repo.loadSchedule(), repo.loadTodos())
        }
    }

    override fun onStatus(status: DeviceStatus) {
        val bat = if (status.soc >= 0) "${status.soc}%" else "--"
        binding.statusText.text =
            "已连接 FoloPassport · 电量 $bat" +
            (if (status.recording) " · 录音中" else "")
    }

    override fun onTodoToggle(id: Int, done: Boolean) {
        repo.applyTodoToggle(id, done)
        refreshLists()
    }

    override fun onRecordingStarted() {
        binding.statusText.text = "正在接收录音..."
    }

    override fun onRecordingFinished(fileName: String?, durationMs: Long, droppedBytes: Long) {
        refreshLists()
        toast("已保存到 音乐/AI Passport: $fileName (${durationMs / 1000}s)")
    }

    override fun onError(message: String) {
        binding.statusText.text = message
        toast(message)
    }

    // ---------------- 列表适配器 ----------------
    private class ScheduleVH(v: View) : RecyclerView.ViewHolder(v) {
        val time = v.findViewById<android.widget.TextView>(R.id.item_time)
        val title = v.findViewById<android.widget.TextView>(R.id.item_title)
        val delete = v.findViewById<Button>(R.id.item_delete)
    }

    private inner class ScheduleAdapter : RecyclerView.Adapter<ScheduleVH>() {
        private val items = mutableListOf<ScheduleItem>()
        fun submitList(list: List<ScheduleItem>) {
            items.clear(); items.addAll(list); notifyDataSetChanged()
        }
        override fun getItemCount() = items.size
        override fun onCreateViewHolder(p: ViewGroup, vt: Int) =
            ScheduleVH(layoutInflater.inflate(R.layout.item_schedule, p, false))
        override fun onBindViewHolder(h: ScheduleVH, pos: Int) {
            val item = items[pos]
            h.time.text = String.format(
                "%02d:%02d - %02d:%02d",
                item.startMin / 60,
                item.startMin % 60,
                item.endMin / 60,
                item.endMin % 60,
            )
            h.title.text = item.title
            h.delete.setOnClickListener { confirmDeleteSchedule(item) }
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
}
