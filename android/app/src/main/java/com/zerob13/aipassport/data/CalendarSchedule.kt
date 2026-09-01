package com.zerob13.aipassport.data

import com.zerob13.aipassport.proto.ScheduleItem
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.ZoneOffset

data class CalendarEventRecord(
    val eventId: Long,
    val beginMs: Long,
    val endMs: Long,
    val title: String,
    val allDay: Boolean,
)

object CalendarSchedule {
    data class Window(val beginMs: Long, val endExclusiveMs: Long)

    fun importWindow(
        nowMs: Long,
        zoneId: ZoneId,
        pastDays: Int,
        futureDays: Int,
    ): Window {
        require(pastDays >= 0 && futureDays >= 0)
        val today = Instant.ofEpochMilli(nowMs).atZone(zoneId).toLocalDate()
        return Window(
            today.minusDays(pastDays.toLong()).atStartOfDay(zoneId).toInstant().toEpochMilli(),
            today.plusDays(futureDays.toLong() + 1).atStartOfDay(zoneId)
                .toInstant().toEpochMilli(),
        )
    }

    fun todayItems(
        events: List<CalendarEventRecord>,
        nowMs: Long,
        zoneId: ZoneId,
        maxItems: Int = 32,
    ): List<ScheduleItem> {
        require(maxItems > 0)
        val today = Instant.ofEpochMilli(nowMs).atZone(zoneId).toLocalDate()
        val dayStart = today.atStartOfDay(zoneId).toInstant().toEpochMilli()
        val dayEnd = today.plusDays(1).atStartOfDay(zoneId).toInstant().toEpochMilli()

        return events.asSequence()
            .filter { event -> overlapsDay(event, today, dayStart, dayEnd) }
            .sortedWith(compareBy<CalendarEventRecord> { it.allDay.not() }.thenBy { it.beginMs })
            .take(maxItems)
            .mapIndexed { index, event ->
                val startMin = if (event.allDay) {
                    0
                } else {
                    minuteOfDay(event.beginMs.coerceAtLeast(dayStart), zoneId).coerceAtMost(1438)
                }
                val endMin = if (event.allDay || event.endMs >= dayEnd) {
                    23 * 60 + 59
                } else {
                    minuteOfDay(event.endMs, zoneId).coerceIn(startMin + 1, 1439)
                }
                ScheduleItem(index + 1, startMin, endMin, event.title)
            }
            .toList()
    }

    private fun overlapsDay(
        event: CalendarEventRecord,
        today: LocalDate,
        dayStart: Long,
        dayEnd: Long,
    ): Boolean {
        if (!event.allDay) return event.beginMs < dayEnd && event.endMs > dayStart
        val startDate = Instant.ofEpochMilli(event.beginMs).atZone(ZoneOffset.UTC).toLocalDate()
        val endDate = Instant.ofEpochMilli(event.endMs).atZone(ZoneOffset.UTC).toLocalDate()
        return !today.isBefore(startDate) && today.isBefore(endDate)
    }

    private fun minuteOfDay(millis: Long, zoneId: ZoneId): Int {
        val time = Instant.ofEpochMilli(millis).atZone(zoneId)
        return time.hour * 60 + time.minute
    }
}
