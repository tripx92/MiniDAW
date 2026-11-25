package com.minidaw.app

import android.content.Context
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.provider.OpenableColumns
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import java.nio.ByteOrder
import kotlin.coroutines.coroutineContext

/** Interleaved stereo float PCM at 48 kHz. */
class DecodedAudio(val data: FloatArray, val frames: Int, val name: String)

/** Background decoder: MediaExtractor + MediaCodec -> float PCM, resampled to 48 kHz stereo. */
object AudioDecoder {
    private const val TARGET_RATE = 48000
    private const val MAX_SECONDS = 120

    suspend fun decode(context: Context, uri: Uri): DecodedAudio = withContext(Dispatchers.Default) {
        val name = displayName(context, uri)
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            extractor.setDataSource(context, uri, null)
            var trackIndex = -1
            var format: MediaFormat? = null
            for (i in 0 until extractor.trackCount) {
                val f = extractor.getTrackFormat(i)
                val mime = f.getString(MediaFormat.KEY_MIME)
                if (mime != null && mime.startsWith("audio/")) {
                    trackIndex = i
                    format = f
                    break
                }
            }
            require(trackIndex >= 0 && format != null) { "No audio track found" }
            extractor.selectTrack(trackIndex)

            var sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            var channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            var encoding = AudioFormat.ENCODING_PCM_16BIT

            val c = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!)
            codec = c
            c.configure(format, null, null, 0)
            c.start()

            var acc = FloatArray(sampleRate * 2 * 8)
            var accLen = 0   // number of floats used
            val maxFloats = sampleRate.toLong() * 2L * MAX_SECONDS
            val info = MediaCodec.BufferInfo()
            var inputDone = false
            var outputDone = false

            while (!outputDone) {
                coroutineContext.ensureActive()
                if (!inputDone) {
                    val ii = c.dequeueInputBuffer(10_000)
                    if (ii >= 0) {
                        val buf = c.getInputBuffer(ii)!!
                        val size = extractor.readSampleData(buf, 0)
                        if (size < 0) {
                            c.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            c.queueInputBuffer(ii, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val oi = c.dequeueOutputBuffer(info, 10_000)
                when {
                    oi >= 0 -> {
                        val ob = c.getOutputBuffer(oi)!!
                        ob.position(info.offset)
                        ob.limit(info.offset + info.size)
                        ob.order(ByteOrder.nativeOrder())
                        val ch = maxOf(channels, 1)
                        if (encoding == AudioFormat.ENCODING_PCM_FLOAT) {
                            val fb = ob.asFloatBuffer()
                            val frames = fb.remaining() / ch
                            if (accLen + frames * 2 > acc.size) acc = acc.copyOf(maxOf(acc.size * 2, accLen + frames * 2))
                            for (f in 0 until frames) {
                                val l = fb.get()
                                val r = if (ch > 1) fb.get() else l
                                for (k in 2 until ch) fb.get()
                                acc[accLen++] = l
                                acc[accLen++] = r
                            }
                        } else {
                            val sb = ob.asShortBuffer()
                            val frames = sb.remaining() / ch
                            if (accLen + frames * 2 > acc.size) acc = acc.copyOf(maxOf(acc.size * 2, accLen + frames * 2))
                            for (f in 0 until frames) {
                                val l = sb.get() / 32768f
                                val r = if (ch > 1) sb.get() / 32768f else l
                                for (k in 2 until ch) sb.get()
                                acc[accLen++] = l
                                acc[accLen++] = r
                            }
                        }
                        c.releaseOutputBuffer(oi, false)
                        if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outputDone = true
                        if (accLen >= maxFloats) outputDone = true
                    }
                    oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val nf = c.outputFormat
                        sampleRate = nf.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        channels = nf.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                        encoding = if (nf.containsKey(MediaFormat.KEY_PCM_ENCODING))
                            nf.getInteger(MediaFormat.KEY_PCM_ENCODING) else AudioFormat.ENCODING_PCM_16BIT
                    }
                }
            }

            val srcFrames = accLen / 2
            require(srcFrames > 0) { "Decoded file is empty" }
            if (sampleRate == TARGET_RATE) {
                DecodedAudio(acc.copyOf(accLen), srcFrames, name)
            } else {
                val ratio = sampleRate.toDouble() / TARGET_RATE
                val outFrames = (srcFrames / ratio).toInt()
                val out = FloatArray(outFrames * 2)
                for (i in 0 until outFrames) {
                    val p = i * ratio
                    val i0 = p.toInt().coerceAtMost(srcFrames - 1)
                    val i1 = (i0 + 1).coerceAtMost(srcFrames - 1)
                    val fr = (p - i0).toFloat()
                    out[2 * i] = acc[2 * i0] + (acc[2 * i1] - acc[2 * i0]) * fr
                    out[2 * i + 1] = acc[2 * i0 + 1] + (acc[2 * i1 + 1] - acc[2 * i0 + 1]) * fr
                }
                DecodedAudio(out, outFrames, name)
            }
        } finally {
            try { codec?.stop() } catch (_: Exception) {}
            try { codec?.release() } catch (_: Exception) {}
            extractor.release()
        }
    }

    private fun displayName(context: Context, uri: Uri): String {
        var result: String? = null
        try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cur ->
                if (cur.moveToFirst()) result = cur.getString(0)
            }
        } catch (_: Exception) {
        }
        val raw = result ?: uri.lastPathSegment ?: "Sample"
        val dot = raw.lastIndexOf('.')
        return if (dot > 0) raw.substring(0, dot) else raw
    }
}
