package com.zerob13.aipassport.data

import com.zerob13.aipassport.proto.ScheduleItem
import com.zerob13.aipassport.proto.SyncProtocol
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset
import kotlin.math.abs

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

    fun deviceItems(
        events: List<CalendarEventRecord>,
        nowMs: Long,
        zoneId: ZoneId,
        maxItems: Int = SyncProtocol.MAX_SCHEDULE_ITEMS,
    ): List<ScheduleItem> {
        require(maxItems > 0)
        val today = Instant.ofEpochMilli(nowMs).atZone(zoneId).toLocalDate()
        val todayEpochDay = today.toEpochDay()
        val calendarOrder = compareBy<ScheduleItem> { it.epochDay }
            .thenBy { !it.allDay }
            .thenBy { it.startMin }
        val chronological = events.map { event ->
            val eventZone = if (event.allDay) ZoneOffset.UTC else zoneId
            val start = Instant.ofEpochMilli(event.beginMs).atZone(eventZone)
            val end = Instant.ofEpochMilli(event.endMs).atZone(eventZone)
            val startMin = if (event.allDay) 0 else
                (start.hour * 60 + start.minute).coerceAtMost(1438)
            val endMin = if (event.allDay || end.toLocalDate() != start.toLocalDate()) {
                1439
            } else {
                (end.hour * 60 + end.minute).coerceIn(startMin + 1, 1439)
            }
            ScheduleItem(
                id = 0,
                epochDay = start.toLocalDate().toEpochDay().toInt(),
                startMin = startMin,
                endMin = endMin,
                allDay = event.allDay,
                title = event.title,
            )
        }.sortedWith(calendarOrder)

        val selected = if (chronological.size <= maxItems) chronological else {
            chronological.sortedWith(
                compareBy<ScheduleItem> { abs(it.epochDay.toLong() - todayEpochDay) }
                    .thenBy { it.epochDay < todayEpochDay }
                    .thenBy { it.startMin }
            ).take(maxItems).sortedWith(calendarOrder)
        }
        return selected.mapIndexed { index, item -> item.copy(id = index + 1) }
    }
}
