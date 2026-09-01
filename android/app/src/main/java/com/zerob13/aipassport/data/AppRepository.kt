// android/.../data/AppRepository.kt —— 本地数据(日程/Todo/录音记录)与持久化。
package com.zerob13.aipassport.data

import android.content.Context
import android.content.SharedPreferences
import com.zerob13.aipassport.proto.ScheduleItem
import com.zerob13.aipassport.proto.TodoItem
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** 一次已保存的录音记录 */
data class RecordingRecord(
    val fileName: String,
    val sizeBytes: Long,
    val durationMs: Long,
)

class AppRepository(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("aipassport", Context.MODE_PRIVATE)
    private val recordingsDir = File(context.filesDir, "recordings").apply { mkdirs() }

    // ---------------- 日程 ----------------
    fun loadSchedule(): MutableList<ScheduleItem> {
        val list = mutableListOf<ScheduleItem>()
        val raw = prefs.getString("schedule", null) ?: return list
        val arr = JSONArray(raw)
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            list.add(
                ScheduleItem(
                    id = o.getInt("id"),
                    startMin = o.getInt("start"),
                    endMin = o.getInt("end"),
                    title = o.getString("title"),
                )
            )
        }
        return list
    }

    fun saveSchedule(list: List<ScheduleItem>) {
        val arr = JSONArray()
        list.forEach {
            arr.put(
                JSONObject()
                    .put("id", it.id)
                    .put("start", it.startMin)
                    .put("end", it.endMin)
                    .put("title", it.title)
            )
        }
        prefs.edit().putString("schedule", arr.toString()).apply()
    }

    fun addSchedule(startMin: Int, endMin: Int, title: String): ScheduleItem {
        val list = loadSchedule()
        val nextId = (list.maxOfOrNull { it.id } ?: 0) + 1
        val item = ScheduleItem(nextId, startMin, endMin, title)
        list.add(item)
        saveSchedule(list)
        return item
    }

    fun deleteSchedule(id: Int): Boolean {
        val list = loadSchedule()
        val removed = list.removeAll { it.id == id }
        if (removed) saveSchedule(list)
        return removed
    }

    // ---------------- Todo ----------------
    fun loadTodos(): MutableList<TodoItem> {
        val list = mutableListOf<TodoItem>()
        val raw = prefs.getString("todos", null) ?: return list
        val arr = JSONArray(raw)
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            list.add(
                TodoItem(
                    id = o.getInt("id"),
                    done = o.getBoolean("done"),
                    title = o.getString("title"),
                )
            )
        }
        return list
    }

    fun saveTodos(list: List<TodoItem>) {
        val arr = JSONArray()
        list.forEach {
            arr.put(
                JSONObject()
                    .put("id", it.id)
                    .put("done", it.done)
                    .put("title", it.title)
            )
        }
        prefs.edit().putString("todos", arr.toString()).apply()
    }

    fun addTodo(title: String): TodoItem {
        val list = loadTodos()
        val nextId = (list.maxOfOrNull { it.id } ?: 0) + 1
        val item = TodoItem(nextId, false, title)
        list.add(item)
        saveTodos(list)
        return item
    }

    fun deleteTodo(id: Int): Boolean {
        val list = loadTodos()
        val removed = list.removeAll { it.id == id }
        if (removed) saveTodos(list)
        return removed
    }

    /** 设备端勾选回传时应用(后写者胜)。 */
    fun applyTodoToggle(id: Int, done: Boolean) {
        val list = loadTodos()
        list.find { it.id == id }?.let {
            it.done = done
            saveTodos(list)
        }
    }

    // ---------------- 录音记录 ----------------
    fun loadRecordings(): MutableList<RecordingRecord> {
        return recordingsDir.listFiles()
            ?.filter { it.isFile && it.name.endsWith(".adpcm") }
            ?.sortedByDescending { it.lastModified() }
            ?.map { RecordingRecord(it.name, it.length(), 0) }
            ?.toMutableList() ?: mutableListOf()
    }

    fun recordingsDirectory(): File = recordingsDir

    fun deleteRecording(name: String) {
        File(recordingsDir, name).delete()
    }
}
