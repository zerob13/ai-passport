// android/.../MainActivity.kt —— AI Passport 同步 App 主界面。
package com.zerob13.aipassport

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
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
import java.io.File

class MainActivity : AppCompatActivity(), SyncListener {

    private lateinit var binding: ActivityMainBinding
    private lateinit var repo: AppRepository
    private lateinit var ble: BleSyncClient

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
        val input = android.widget.EditText(this).apply {
            hint = "时间 开始-结束 标题,例如 14:30-15:30 会议"
        }
        android.app.AlertDialog.Builder(this)
            .setTitle("新增日程")
            .setView(input)
            .setPositiveButton("添加") { _, _ ->
                val text = input.text.toString().trim()
                parseScheduleText(text)?.let {
                    repo.addSchedule(it.first, it.second, it.third)
                    if (ble.isConnected) ble.pushSnapshot(repo.loadSchedule(), repo.loadTodos())
                    refreshLists()
                } ?: toast("格式: 开始-结束 标题")
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun parseScheduleText(text: String): Triple<Int, Int, String>? {
        val parts = text.split(" ", limit = 2)
        if (parts.size < 2) return null
        val times = parts[0].split("-")
        if (times.size != 2) return null
        val start = parseClock(times[0]) ?: return null
        val end = parseClock(times[1]) ?: return null
        return Triple(start, end, parts[1])
    }

    private fun parseClock(s: String): Int? {
        val p = s.split(":")
        if (p.size != 2) return null
        val h = p[0].toIntOrNull() ?: return null
        val m = p[1].toIntOrNull() ?: return null
        if (h !in 0..23 || m !in 0..59) return null
        return h * 60 + m
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
                    if (ble.isConnected) ble.pushSnapshot(repo.loadSchedule(), repo.loadTodos())
                    refreshLists()
                }
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

    override fun onRecordingFinished(file: File?, durationMs: Long, droppedBytes: Long) {
        refreshLists()
        toast("录音已保存: ${file?.name} (${durationMs / 1000}s)")
    }

    override fun onError(message: String) {
        binding.statusText.text = message
        toast(message)
    }

    // ---------------- 列表适配器 ----------------
    private class ScheduleVH(v: View) : RecyclerView.ViewHolder(v) {
        val time = v.findViewById<android.widget.TextView>(R.id.item_time)
        val title = v.findViewById<android.widget.TextView>(R.id.item_title)
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
            val it = items[pos]
            h.time.text = String.format(
                "%02d:%02d - %02d:%02d",
                it.startMin / 60, it.startMin % 60, it.endMin / 60, it.endMin % 60
            )
            h.title.text = it.title
        }
    }

    private class TodoVH(v: View) : RecyclerView.ViewHolder(v) {
        val check = v.findViewById<android.widget.CheckBox>(R.id.item_check)
        val title = v.findViewById<android.widget.TextView>(R.id.item_title)
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
            val it = items[pos]
            h.check.isChecked = it.done
            h.title.text = it.title
            h.title.alpha = if (it.done) 0.5f else 1f
            h.check.setOnCheckedChangeListener { _, checked ->
                if (checked != it.done) {
                    repo.toggleTodo(it.id)
                    ble.sendTodo(repo.loadTodos().first { t -> t.id == it.id })
                    updateCounters()
                }
            }
        }
    }

    private class RecordingVH(v: View) : RecyclerView.ViewHolder(v) {
        val name = v.findViewById<android.widget.TextView>(R.id.item_name)
        val size = v.findViewById<android.widget.TextView>(R.id.item_size)
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
            val it = items[pos]
            h.name.text = it.fileName
            h.size.text = String.format("%.1f KB", it.sizeBytes / 1024.0)
        }
    }

    companion object {
        private val dummy = arrayOf<ScheduleItem>() // 保持导入
    }
}
