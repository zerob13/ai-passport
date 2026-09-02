package com.zerob13.aipassport

import com.zerob13.aipassport.data.CalendarEventRecord
import com.zerob13.aipassport.data.CalendarSchedule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.time.LocalDate
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
    fun deviceItemsKeepImportedDatesAndCalendarOrder() {
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

        val items = CalendarSchedule.deviceItems(events, now, zone)

        assertEquals(listOf("Overnight", "Holiday", "Meeting", "Tomorrow"),
            items.map { it.title })
        assertEquals(epochDay(2026, 8, 31), items[0].epochDay)
        assertEquals(23 * 60 + 30, items[0].startMin)
        assertEquals(1439, items[0].endMin)
        assertTrue(items[1].allDay)
        assertEquals(epochDay(2026, 9, 1), items[1].epochDay)
        assertEquals(9 * 60 + 15, items[2].startMin)
        assertEquals(epochDay(2026, 9, 2), items[3].epochDay)
    }

    @Test
    fun deviceItemsKeepFortyClosestDates() {
        val today = LocalDate.of(2026, 9, 1)
        val events = (-22L..22L).mapIndexed { index, offset ->
            val date = today.plusDays(offset)
            CalendarEventRecord(
                eventId = index.toLong(),
                beginMs = date.atTime(9, 0).atZone(zone).toInstant().toEpochMilli(),
                endMs = date.atTime(10, 0).atZone(zone).toInstant().toEpochMilli(),
                title = "Event $offset",
                allDay = false,
            )
        }

        val items = CalendarSchedule.deviceItems(events, now, zone, maxItems = 40)

        assertEquals(40, items.size)
        assertEquals(today.minusDays(19).toEpochDay().toInt(), items.first().epochDay)
        assertEquals(today.plusDays(20).toEpochDay().toInt(), items.last().epochDay)
        assertTrue(items.any { it.epochDay == today.toEpochDay().toInt() })
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

    private fun epochDay(year: Int, month: Int, day: Int): Int =
        LocalDate.of(year, month, day).toEpochDay().toInt()
}
