package com.zerob13.aipassport

import com.zerob13.aipassport.data.CalendarEventRecord
import com.zerob13.aipassport.data.CalendarSchedule
import org.junit.Assert.assertEquals
import org.junit.Test
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset

class CalendarScheduleTest {
    private val zone = ZoneId.of("Asia/Shanghai")
    private val now = localMillis(2026, 9, 1, 12, 0)

    @Test
    fun importWindowIncludesSelectedPastAndFutureDays() {
        val window = CalendarSchedule.importWindow(now, zone, pastDays = 2, futureDays = 3)

        assertEquals(localMillis(2026, 8, 30, 0, 0), window.beginMs)
        assertEquals(localMillis(2026, 9, 5, 0, 0), window.endExclusiveMs)
    }

    @Test
    fun todayItemsSelectAndClampCalendarInstances() {
        val events = listOf(
            event(1, 2026, 8, 31, 23, 30, 2026, 9, 1, 0, 30, "Overnight"),
            event(2, 2026, 9, 1, 9, 15, 2026, 9, 1, 10, 0, "Meeting"),
            event(3, 2026, 9, 2, 9, 0, 2026, 9, 2, 10, 0, "Tomorrow"),
            CalendarEventRecord(
                eventId = 4,
                beginMs = utcMillis(2026, 9, 1, 0, 0),
                endMs = utcMillis(2026, 9, 2, 0, 0),
                title = "Holiday",
                allDay = true,
            ),
        )

        val items = CalendarSchedule.todayItems(events, now, zone)

        assertEquals(listOf("Holiday", "Overnight", "Meeting"), items.map { it.title })
        assertEquals(0, items[0].startMin)
        assertEquals(1439, items[0].endMin)
        assertEquals(0, items[1].startMin)
        assertEquals(30, items[1].endMin)
        assertEquals(9 * 60 + 15, items[2].startMin)
        assertEquals(10 * 60, items[2].endMin)
    }

    private fun event(
        id: Long,
        startYear: Int,
        startMonth: Int,
        startDay: Int,
        startHour: Int,
        startMinute: Int,
        endYear: Int,
        endMonth: Int,
        endDay: Int,
        endHour: Int,
        endMinute: Int,
        title: String,
    ) = CalendarEventRecord(
        id,
        localMillis(startYear, startMonth, startDay, startHour, startMinute),
        localMillis(endYear, endMonth, endDay, endHour, endMinute),
        title,
        false,
    )

    private fun localMillis(year: Int, month: Int, day: Int, hour: Int, minute: Int): Long =
        LocalDateTime.of(year, month, day, hour, minute).atZone(zone).toInstant().toEpochMilli()

    private fun utcMillis(year: Int, month: Int, day: Int, hour: Int, minute: Int): Long =
        LocalDateTime.of(year, month, day, hour, minute)
            .atZone(ZoneOffset.UTC).toInstant().toEpochMilli()
}
