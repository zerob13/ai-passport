// android/.../data/AppRepository.kt —— 本地数据(日程/Todo/录音记录)与持久化。
package com.zerob13.aipassport.data

import android.content.ContentUris
import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.os.Build
import android.provider.CalendarContract
import android.provider.MediaStore
import com.zerob13.aipassport.audio.WavRecording
import com.zerob13.aipassport.proto.ScheduleItem
import com.zerob13.aipassport.proto.TodoItem
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.time.ZoneId

/** 一次已保存的录音记录 */
data class RecordingRecord(
    val fileName: String,
    val sizeBytes: Long,
    val durationMs: Long,
    val contentUri: Uri?,
    val filePath: String?,
    val locationLabel: String,
)

class AppRepository(context: Context) {
    private val appContext = context.applicationContext
    private val prefs: SharedPreferences =
        appContext.getSharedPreferences("aipassport", Context.MODE_PRIVATE)
    private val recordingsDir = File(appContext.filesDir, "recordings").apply { mkdirs() }

    init {
        migrateLegacyRecordings()
    }

    // ---------------- Calendar ----------------
    fun loadCalendarEvents(): MutableList<CalendarEventRecord> {
        val list = mutableListOf<CalendarEventRecord>()
        val raw = prefs.getString(KEY_CALENDAR_EVENTS, null) ?: return list
        val arr = JSONArray(raw)
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            list.add(
                CalendarEventRecord(
                    eventId = o.getLong("id"),
                    beginMs = o.getLong("begin"),
                    endMs = o.getLong("end"),
                    title = o.getString("title"),
                    allDay = o.getBoolean("allDay"),
                )
            )
        }
        return list
    }

    fun calendarRange(): Pair<Int, Int> =
        prefs.getInt(KEY_CALENDAR_PAST_DAYS, DEFAULT_PAST_DAYS) to
            prefs.getInt(KEY_CALENDAR_FUTURE_DAYS, DEFAULT_FUTURE_DAYS)

    /** Queries visible recurring and one-off calendar instances. Call from a worker thread. */
    fun importCalendar(
        pastDays: Int,
        futureDays: Int,
        nowMs: Long = System.currentTimeMillis(),
    ): List<CalendarEventRecord> {
        val window = CalendarSchedule.importWindow(
            nowMs,
            ZoneId.systemDefault(),
            pastDays,
            futureDays,
        )
        val projection = arrayOf(
            CalendarContract.Instances.EVENT_ID,
            CalendarContract.Instances.BEGIN,
            CalendarContract.Instances.END,
            CalendarContract.Instances.TITLE,
            CalendarContract.Instances.ALL_DAY,
        )
        val events = mutableListOf<CalendarEventRecord>()
        CalendarContract.Instances.query(
            appContext.contentResolver,
            projection,
            window.beginMs,
            window.endExclusiveMs,
        )?.use { cursor ->
            val idColumn = cursor.getColumnIndexOrThrow(CalendarContract.Instances.EVENT_ID)
            val beginColumn = cursor.getColumnIndexOrThrow(CalendarContract.Instances.BEGIN)
            val endColumn = cursor.getColumnIndexOrThrow(CalendarContract.Instances.END)
            val titleColumn = cursor.getColumnIndexOrThrow(CalendarContract.Instances.TITLE)
            val allDayColumn = cursor.getColumnIndexOrThrow(CalendarContract.Instances.ALL_DAY)
            while (cursor.moveToNext()) {
                val title = cursor.getString(titleColumn).orEmpty().trim()
                val begin = cursor.getLong(beginColumn)
                val end = cursor.getLong(endColumn)
                if (title.isEmpty() || end <= begin) continue
                events.add(
                    CalendarEventRecord(
                        eventId = cursor.getLong(idColumn),
                        beginMs = begin,
                        endMs = end,
                        title = title,
                        allDay = cursor.getInt(allDayColumn) != 0,
                    )
                )
            }
        }
        val imported = events
            .distinctBy { Triple(it.eventId, it.beginMs, it.endMs) }
            .sortedBy { it.beginMs }
        saveCalendarEvents(imported, pastDays, futureDays)
        return imported
    }

    fun loadTodaySchedule(nowMs: Long = System.currentTimeMillis()): List<ScheduleItem> =
        CalendarSchedule.todayItems(
            loadCalendarEvents(),
            nowMs,
            ZoneId.systemDefault(),
        )

    private fun saveCalendarEvents(
        list: List<CalendarEventRecord>,
        pastDays: Int,
        futureDays: Int,
    ) {
        val arr = JSONArray()
        list.forEach {
            arr.put(
                JSONObject()
                    .put("id", it.eventId)
                    .put("begin", it.beginMs)
                    .put("end", it.endMs)
                    .put("title", it.title)
                    .put("allDay", it.allDay)
            )
        }
        prefs.edit()
            .putString(KEY_CALENDAR_EVENTS, arr.toString())
            .putInt(KEY_CALENDAR_PAST_DAYS, pastDays)
            .putInt(KEY_CALENDAR_FUTURE_DAYS, futureDays)
            .apply()
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val records = mutableListOf<RecordingRecord>()
            val collection = MediaStore.Audio.Media.EXTERNAL_CONTENT_URI
            val projection = arrayOf(
                MediaStore.Audio.Media._ID,
                MediaStore.Audio.Media.DISPLAY_NAME,
                MediaStore.Audio.Media.SIZE,
                MediaStore.Audio.Media.DURATION,
            )
            appContext.contentResolver.query(
                collection,
                projection,
                "${MediaStore.Audio.Media.RELATIVE_PATH} = ? AND " +
                    "${MediaStore.Audio.Media.MIME_TYPE} = ?",
                arrayOf(WavRecording.RELATIVE_PATH, "audio/wav"),
                "${MediaStore.Audio.Media.DATE_ADDED} DESC",
            )?.use { cursor ->
                val idColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media._ID)
                val nameColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.DISPLAY_NAME)
                val sizeColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.SIZE)
                val durationColumn = cursor.getColumnIndexOrThrow(MediaStore.Audio.Media.DURATION)
                while (cursor.moveToNext()) {
                    val size = cursor.getLong(sizeColumn)
                    var duration = cursor.getLong(durationColumn)
                    if (duration <= 0 && size > 44) {
                        duration = (size - 44) * 1000 / (16_000 * 2)
                    }
                    records.add(
                        RecordingRecord(
                            fileName = cursor.getString(nameColumn),
                            sizeBytes = size,
                            durationMs = duration,
                            contentUri = ContentUris.withAppendedId(
                                collection,
                                cursor.getLong(idColumn),
                            ),
                            filePath = null,
                            locationLabel = WavRecording.LOCATION_LABEL,
                        )
                    )
                }
            }
            return records
        }

        return recordingsDir.listFiles()
            ?.filter { it.isFile && it.name.endsWith(".wav") }
            ?.sortedByDescending { it.lastModified() }
            ?.map {
                val size = it.length()
                RecordingRecord(
                    fileName = it.name,
                    sizeBytes = size,
                    durationMs = if (size > 44) (size - 44) * 1000 / (16_000 * 2) else 0,
                    contentUri = null,
                    filePath = it.absolutePath,
                    locationLabel = "应用内录音",
                )
            }
            ?.toMutableList() ?: mutableListOf()
    }

    fun deleteRecording(record: RecordingRecord): Boolean {
        val uri = record.contentUri
        if (uri != null) return appContext.contentResolver.delete(uri, null, null) > 0
        return record.filePath?.let { File(it).delete() } ?: false
    }

    private fun migrateLegacyRecordings() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        // ponytail: synchronous one-time migration is enough for short BLE recordings;
        // move it to WorkManager only if multi-hour legacy files become realistic.
        recordingsDir.listFiles()
            ?.filter { it.isFile && it.length() > 0 && it.name.endsWith(".adpcm") }
            ?.forEach { source ->
                val target = WavRecording.create(
                    appContext,
                    source.name.removeSuffix(".adpcm") + ".wav",
                    16_000,
                ) ?: return@forEach
                val converted = runCatching {
                    source.inputStream().buffered().use { input ->
                        val buffer = ByteArray(8192)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            val chunk = if (count == buffer.size) buffer else buffer.copyOf(count)
                            if (!target.writeAdpcm(chunk)) return@runCatching false
                        }
                    }
                    target.finish()
                }.getOrDefault(false)
                if (converted) source.delete() else target.cancel()
            }
    }

    private companion object {
        const val KEY_CALENDAR_EVENTS = "calendar_events"
        const val KEY_CALENDAR_PAST_DAYS = "calendar_past_days"
        const val KEY_CALENDAR_FUTURE_DAYS = "calendar_future_days"
        const val DEFAULT_PAST_DAYS = 7
        const val DEFAULT_FUTURE_DAYS = 30
    }
}
