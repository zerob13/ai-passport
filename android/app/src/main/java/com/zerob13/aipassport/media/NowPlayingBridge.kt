// Android MediaSession -> DimOS Now Playing bridge. No player-specific SDK is required.
package com.zerob13.aipassport.media

import android.content.ComponentName
import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import androidx.core.app.NotificationManagerCompat
import com.zerob13.aipassport.proto.NowPlaying
import com.zerob13.aipassport.proto.SyncProtocol
import java.util.Locale

class NowPlayingBridge(
    private val context: Context,
    private val onTrack: (NowPlaying) -> Unit,
    private val onProgress: (positionMs: Long, durationMs: Long, playing: Boolean) -> Unit,
    private val onClear: () -> Unit,
) {
    private val handler = Handler(Looper.getMainLooper())
    private val manager = context.getSystemService(MediaSessionManager::class.java)
    private val listenerComponent = ComponentName(context, MediaListenerService::class.java)
    private var started = false
    private var controller: MediaController? = null
    private var current: NowPlaying? = null
    private var lastTrackKey: String? = null

    private val sessionsListener =
        MediaSessionManager.OnActiveSessionsChangedListener { sessions ->
            selectController(sessions.orEmpty())
        }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) {
            publishTrack()
        }

        override fun onPlaybackStateChanged(state: PlaybackState?) {
            publishProgress()
        }

        override fun onSessionDestroyed() {
            refreshSessions()
        }
    }

    private val progressTick = object : Runnable {
        override fun run() {
            if (!started) return
            publishProgress()
            handler.postDelayed(this, PROGRESS_INTERVAL_MS)
        }
    }

    fun hasAccess(): Boolean =
        context.packageName in NotificationManagerCompat.getEnabledListenerPackages(context)

    fun start() {
        if (started || !hasAccess()) return
        started = true
        try {
            manager.addOnActiveSessionsChangedListener(
                sessionsListener,
                listenerComponent,
                handler,
            )
            NotificationListenerService.requestRebind(listenerComponent)
            refreshSessions()
            // Notification access can become usable just after returning from Settings.
            handler.postDelayed(::refreshSessions, LISTENER_BIND_DELAY_MS)
            handler.post(progressTick)
        } catch (_: SecurityException) {
            stop()
            onClear()
        }
    }

    fun stop() {
        if (!started) return
        started = false
        handler.removeCallbacks(progressTick)
        handler.removeCallbacksAndMessages(null)
        runCatching { manager.removeOnActiveSessionsChangedListener(sessionsListener) }
        controller?.unregisterCallback(controllerCallback)
        controller = null
        current = null
        lastTrackKey = null
    }

    fun currentSnapshot(): NowPlaying? {
        val item = current ?: return null
        return item.copy(
            positionMs = currentPosition(item.durationMs),
            playing = isPlaying(),
        )
    }

    private fun refreshSessions() {
        if (!started) return
        val sessions = try {
            manager.getActiveSessions(listenerComponent)
        } catch (_: SecurityException) {
            emptyList()
        }
        selectController(sessions)
    }

    private fun selectController(sessions: List<MediaController>) {
        val next = sessions.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: sessions.firstOrNull { it.metadata != null }
        if (controller?.sessionToken == next?.sessionToken) {
            publishTrack()
            return
        }

        controller?.unregisterCallback(controllerCallback)
        controller = next
        current = null
        lastTrackKey = null
        if (next == null) {
            onClear()
            return
        }
        next.registerCallback(controllerCallback, handler)
        publishTrack()
    }

    @Suppress("DEPRECATION")
    private fun publishTrack() {
        val activeController = controller ?: return
        val metadata = activeController.metadata ?: run {
            current = null
            lastTrackKey = null
            onClear()
            return
        }
        val title = metadata.getString(MediaMetadata.METADATA_KEY_TITLE)
            ?: metadata.getString(MediaMetadata.METADATA_KEY_DISPLAY_TITLE)
            ?: "UNKNOWN TRACK"
        val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: metadata.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST)
            ?: "UNKNOWN ARTIST"
        val album = metadata.getString(MediaMetadata.METADATA_KEY_ALBUM).orEmpty()
        val duration = metadata.getLong(MediaMetadata.METADATA_KEY_DURATION).coerceAtLeast(0)
        val artwork = metadata.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: metadata.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: metadata.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
        val key = listOf(
            activeController.packageName,
            metadata.getString(MediaMetadata.METADATA_KEY_MEDIA_ID).orEmpty(),
            title,
            artist,
            album,
            duration,
            artwork?.generationId ?: 0,
        ).joinToString("\u0000")

        if (key == lastTrackKey) {
            publishProgress()
            return
        }
        lastTrackKey = key
        val item = NowPlaying(
            title = title,
            artist = artist,
            album = album,
            source = sourceLabel(activeController.packageName),
            durationMs = duration,
            positionMs = currentPosition(duration),
            playing = isPlaying(),
            artworkRgb565 = artwork?.let(::artworkToRgb565),
        )
        current = item
        onTrack(item)
    }

    private fun publishProgress() {
        val item = current ?: return
        val position = currentPosition(item.durationMs)
        val playing = isPlaying()
        current = item.copy(positionMs = position, playing = playing)
        onProgress(position, item.durationMs, playing)
    }

    private fun isPlaying(): Boolean =
        controller?.playbackState?.state == PlaybackState.STATE_PLAYING

    private fun currentPosition(durationMs: Long): Long {
        val state = controller?.playbackState ?: return 0
        var position = state.position.coerceAtLeast(0)
        if (state.state == PlaybackState.STATE_PLAYING && state.lastPositionUpdateTime > 0) {
            val elapsed = (SystemClock.elapsedRealtime() - state.lastPositionUpdateTime)
                .coerceAtLeast(0)
            position += (elapsed * state.playbackSpeed).toLong()
        }
        return if (durationMs > 0) position.coerceIn(0, durationMs) else position
    }

    @Suppress("DEPRECATION")
    private fun sourceLabel(packageName: String): String {
        if (packageName == SPOTIFY_PACKAGE) return "SPOTIFY"
        return runCatching {
            context.packageManager.getApplicationLabel(
                context.packageManager.getApplicationInfo(packageName, 0)
            ).toString()
        }.getOrElse { packageName.substringAfterLast('.') }
            .uppercase(Locale.ROOT)
    }

    private companion object {
        const val SPOTIFY_PACKAGE = "com.spotify.music"
        const val PROGRESS_INTERVAL_MS = 1_000L
        const val LISTENER_BIND_DELAY_MS = 750L
    }
}

/** System permission anchor used by MediaSessionManager.getActiveSessions(). */
class MediaListenerService : NotificationListenerService()

internal fun artworkToRgb565(source: Bitmap): ByteArray {
    val side = minOf(source.width, source.height)
    if (side <= 0) return ByteArray(0)
    val cropped = Bitmap.createBitmap(
        source,
        (source.width - side) / 2,
        (source.height - side) / 2,
        side,
        side,
    )
    val scaled = Bitmap.createScaledBitmap(
        cropped,
        SyncProtocol.MEDIA_ART_WIDTH,
        SyncProtocol.MEDIA_ART_HEIGHT,
        true,
    )
    val pixels = IntArray(SyncProtocol.MEDIA_ART_WIDTH * SyncProtocol.MEDIA_ART_HEIGHT)
    scaled.getPixels(
        pixels,
        0,
        SyncProtocol.MEDIA_ART_WIDTH,
        0,
        0,
        SyncProtocol.MEDIA_ART_WIDTH,
        SyncProtocol.MEDIA_ART_HEIGHT,
    )
    if (scaled !== source) scaled.recycle()
    if (cropped !== source && cropped !== scaled) cropped.recycle()
    return rgb565Bytes(pixels)
}

internal fun rgb565Bytes(pixels: IntArray): ByteArray {
    val out = ByteArray(pixels.size * 2)
    pixels.forEachIndexed { index, color ->
        val rgb565 = (((color ushr 19) and 0x1F) shl 11) or
            (((color ushr 10) and 0x3F) shl 5) or
            ((color ushr 3) and 0x1F)
        out[index * 2] = (rgb565 and 0xFF).toByte()
        out[index * 2 + 1] = (rgb565 ushr 8).toByte()
    }
    return out
}
