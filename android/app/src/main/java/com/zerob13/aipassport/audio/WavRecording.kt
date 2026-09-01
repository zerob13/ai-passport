package com.zerob13.aipassport.audio

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

internal class ImaAdpcmDecoder {
    private var predictor = 0
    private var index = 0

    fun decode(input: ByteArray): ByteArray {
        val output = ByteArray(input.size * 4)
        var offset = 0
        input.forEach { packed ->
            offset = putSample(output, offset, decodeNibble(packed.toInt() and 0x0F))
            offset = putSample(output, offset, decodeNibble((packed.toInt() ushr 4) and 0x0F))
        }
        return output
    }

    private fun decodeNibble(nibble: Int): Int {
        val step = STEPS[index]
        var difference = step ushr 3
        if (nibble and 4 != 0) difference += step
        if (nibble and 2 != 0) difference += step ushr 1
        if (nibble and 1 != 0) difference += step ushr 2

        predictor += if (nibble and 8 != 0) -difference else difference
        predictor = predictor.coerceIn(Short.MIN_VALUE.toInt(), Short.MAX_VALUE.toInt())
        index = (index + INDEX_CHANGES[nibble]).coerceIn(0, STEPS.lastIndex)
        return predictor
    }

    private fun putSample(output: ByteArray, offset: Int, sample: Int): Int {
        output[offset] = (sample and 0xFF).toByte()
        output[offset + 1] = ((sample ushr 8) and 0xFF).toByte()
        return offset + 2
    }

    private companion object {
        val STEPS = intArrayOf(
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
            34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
            143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
            494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
            1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
            4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
            11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
            27086, 29794, 32767,
        )
        val INDEX_CHANGES = intArrayOf(
            -1, -1, -1, -1, 2, 4, 6, 8,
            -1, -1, -1, -1, 2, 4, 6, 8,
        )
    }
}

internal object PcmWav {
    fun header(dataBytes: Long, sampleRate: Int, channels: Int = 1): ByteArray {
        val bitsPerSample = 16
        val blockAlign = channels * bitsPerSample / 8
        return ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN).apply {
            put("RIFF".encodeToByteArray())
            putInt((36 + dataBytes).toInt())
            put("WAVEfmt ".encodeToByteArray())
            putInt(16)
            putShort(1.toShort())
            putShort(channels.toShort())
            putInt(sampleRate)
            putInt(sampleRate * blockAlign)
            putShort(blockAlign.toShort())
            putShort(bitsPerSample.toShort())
            put("data".encodeToByteArray())
            putInt(dataBytes.toInt())
        }.array()
    }
}

internal class WavRecording private constructor(
    private val context: Context,
    val fileName: String,
    private val sampleRate: Int,
    private val output: FileOutputStream,
    private val uri: Uri?,
    private val file: File?,
) {
    private val decoder = ImaAdpcmDecoder()
    private var dataBytes = 0L
    private var closed = false

    fun writeAdpcm(data: ByteArray): Boolean = try {
        val pcm = decoder.decode(data)
        output.write(pcm)
        dataBytes += pcm.size
        true
    } catch (_: Exception) {
        cancel()
        false
    }

    fun finish(): Boolean {
        if (closed) return false
        return try {
            output.flush()
            output.channel.position(0)
            output.write(PcmWav.header(dataBytes, sampleRate))
            output.close()
            closed = true
            val contentUri = uri
            if (contentUri != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Audio.Media.IS_PENDING, 0)
                }
                check(context.contentResolver.update(contentUri, values, null, null) == 1)
            }
            true
        } catch (_: Exception) {
            cancel()
            false
        }
    }

    fun cancel() {
        if (!closed) {
            runCatching { output.close() }
            closed = true
        }
        if (uri != null) {
            runCatching { context.contentResolver.delete(uri, null, null) }
        } else {
            file?.delete()
        }
    }

    companion object {
        const val RELATIVE_PATH = "Music/AI Passport/"
        const val LOCATION_LABEL = "音乐/AI Passport"

        fun create(context: Context, fileName: String, sampleRate: Int): WavRecording? {
            val appContext = context.applicationContext
            var uri: Uri? = null
            var file: File? = null
            var stream: FileOutputStream? = null
            return try {
                val output = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    val values = ContentValues().apply {
                        put(MediaStore.Audio.Media.DISPLAY_NAME, fileName)
                        put(MediaStore.Audio.Media.MIME_TYPE, "audio/wav")
                        put(MediaStore.Audio.Media.RELATIVE_PATH, RELATIVE_PATH)
                        put(MediaStore.Audio.Media.IS_PENDING, 1)
                    }
                    uri = appContext.contentResolver.insert(
                        MediaStore.Audio.Media.EXTERNAL_CONTENT_URI,
                        values,
                    ) ?: error("Unable to create MediaStore recording")
                    val descriptor = appContext.contentResolver.openFileDescriptor(uri!!, "rw")
                        ?: error("Unable to open MediaStore recording")
                    ParcelFileDescriptor.AutoCloseOutputStream(descriptor)
                } else {
                    val directory = File(appContext.filesDir, "recordings").apply { mkdirs() }
                    val destination = File(directory, fileName)
                    file = destination
                    FileOutputStream(destination)
                }
                stream = output
                output.write(ByteArray(44))
                WavRecording(appContext, fileName, sampleRate, output, uri, file)
            } catch (_: Exception) {
                runCatching { stream?.close() }
                if (uri != null) {
                    runCatching { appContext.contentResolver.delete(uri!!, null, null) }
                }
                file?.delete()
                null
            }
        }
    }
}
