package com.minidaw.app

/** Thin JNI facade over the C++ engine (libminidaw.so). All calls are non-blocking. */
object NativeEngine {
    init {
        System.loadLibrary("minidaw")
    }

    // Track parameter ids (must match enum TrackParam in AudioEngine.h)
    const val TP_VOLUME = 0
    const val TP_PAN = 1
    const val TP_SEND_A = 2
    const val TP_SEND_B = 3
    const val TP_ACTIVE = 4
    const val TP_SOLO = 5
    const val TP_XF_ASSIGN = 6
    const val TP_FILTER_TYPE = 7
    const val TP_CUTOFF = 8
    const val TP_RESONANCE = 9
    const val TP_DRIVE = 10

    // Global parameter ids (must match enum GlobalParam)
    const val GP_MASTER_VOL = 0
    const val GP_RET_A_VOL = 1
    const val GP_RET_B_VOL = 2
    const val GP_CROSSFADER = 3
    const val GP_DELAY_MODE = 4
    const val GP_CLICK_VOL = 5
    const val GP_METRONOME = 6
    const val GP_BPM = 7
    const val GP_QUANT = 8

    external fun nativeCreate()
    external fun nativeStart(): Boolean
    external fun nativeStop()
    external fun nativeDestroy()

    external fun nativePlay()
    external fun nativeStopTransport()
    external fun nativeSlotPress(track: Int, scene: Int)
    external fun nativeLaunchScene(scene: Int)
    external fun nativeStopTrack(track: Int)
    external fun nativeStopAll()
    external fun nativeSetArm(track: Int, armed: Boolean)

    external fun nativeSetGlobal(id: Int, value: Float)
    external fun nativeSetTrackParam(track: Int, id: Int, value: Float)
    external fun nativeSetClipParams(track: Int, scene: Int, warp: Int, bars: Int, transpose: Int)
    external fun nativeLoadClip(track: Int, scene: Int, data: FloatArray, frames: Int, name: String): Boolean

    external fun nativeGetStatus(out: FloatArray)
    external fun nativeGetMeters(out: FloatArray)
    external fun nativePollEvent(out: IntArray): Boolean
    external fun nativeClipName(track: Int, scene: Int): String
    external fun nativeClipInfo(track: Int, scene: Int, out: IntArray)
}
