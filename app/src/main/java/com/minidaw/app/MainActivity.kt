package com.minidaw.app

import android.Manifest
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.Canvas
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.Locale
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.roundToInt

// ------------------------------------------------------------------ Ableton Live 11 palette
private val BgDark = Color(0xFF1E1E1E)
private val Panel = Color(0xFF262626)
private val CellBg = Color(0xFF333333)
private val EmptyCell = Color(0xFF2B2B2B)
private val Accent = Color(0xFFFF764D)
private val TextMain = Color(0xFFD6D6D6)
private val TextDim = Color(0xFF8C8C8C)
private val Yellow = Color(0xFFFFD02B)
private val SoloBlue = Color(0xFF3DA9FC)
private val QueueGreen = Color(0xFF3FD07A)
private val RecRed = Color(0xFFE5392F)
private val TrackColors = listOf(Color(0xFFF0B429), Color(0xFF2FA3E8), Color(0xFF9B6CE0), Color(0xFFE5578D))

private const val TRACKS = 4
private const val SCENES = 5
private const val COLUMNS = 7   // 4 tracks + Return A + Return B + Master
private const val UNITY_FADER = 60f / 66f
private val QuantLabels = listOf("None", "1/4 Beat", "1 Beat", "1 Bar", "2 Bars")
private val DelayLabels = listOf("1/8", "1/8D", "1/4")

// Fader position (0..1) <-> dB: -60 dB .. +6 dB, below 2% = -inf
private fun faderToLin(p: Float): Float = if (p <= 0.02f) 0f else 10f.pow((-60f + 66f * p) / 20f)
private fun faderDbText(p: Float): String =
    if (p <= 0.02f) "-inf" else String.format(Locale.US, "%.1f", -60f + 66f * p)
private fun linToDbText(v: Float): String =
    if (v < 0.001f) "-inf" else String.format(Locale.US, "%.1f", 20f * log10(v))
private fun meterNorm(v: Float): Float {
    if (v < 0.001f) return 0f
    return ((20f * log10(v) + 60f) / 66f).coerceIn(0f, 1f)
}

// ------------------------------------------------------------------ UI state
class MeterState {
    var peakL by mutableFloatStateOf(0f)
    var peakR by mutableFloatStateOf(0f)
    var rmsL by mutableFloatStateOf(0f)
    var rmsR by mutableFloatStateOf(0f)
}

class TrackUi {
    var fader by mutableFloatStateOf(UNITY_FADER)
    var pan by mutableFloatStateOf(0f)
    var sendA by mutableFloatStateOf(0f)
    var sendB by mutableFloatStateOf(0f)
    var active by mutableStateOf(true)
    var solo by mutableStateOf(false)
    var xf by mutableIntStateOf(1)            // 0 = A, 1 = none, 2 = B
    var armed by mutableStateOf(false)
    var filterType by mutableIntStateOf(0)    // 0 off, 1 LP, 2 HP
    var cutoffPos by mutableFloatStateOf(0.75f)
    var resPos by mutableFloatStateOf(0.2f)
    var drive by mutableFloatStateOf(0f)

    fun cutoffHz() = 20f * 1000f.pow(cutoffPos)
    fun resQ() = 0.5f * 20f.pow(resPos)
}

class DawState {
    var playing by mutableStateOf(false)
    var bpm by mutableFloatStateOf(120f)
    var position by mutableStateOf("001.1.1")
    var metronome by mutableStateOf(false)
    var quantIdx by mutableIntStateOf(3)
    var xfader by mutableFloatStateOf(0.5f)
    var masterFader by mutableFloatStateOf(UNITY_FADER)
    var retAFader by mutableFloatStateOf(UNITY_FADER)
    var retBFader by mutableFloatStateOf(UNITY_FADER)
    var delayMode by mutableIntStateOf(0)
    var clickVol by mutableFloatStateOf(0.5f)
    var blink by mutableStateOf(false)
    var selT by mutableIntStateOf(0)
    var selS by mutableIntStateOf(0)
    var loading by mutableStateOf(false)

    val tracks = List(TRACKS) { TrackUi() }
    val meters = List(7) { MeterState() }
    val states = mutableStateListOf<Int>().apply { repeat(TRACKS * SCENES) { add(0) } }
    val progress = mutableStateListOf<Float>().apply { repeat(TRACKS) { add(0f) } }
    val stopQueued = mutableStateListOf<Boolean>().apply { repeat(TRACKS) { add(false) } }
    val has = mutableStateListOf<Boolean>().apply { repeat(TRACKS * SCENES) { add(false) } }
    val names = mutableStateListOf<String>().apply { repeat(TRACKS * SCENES) { add("") } }
    val bars = mutableStateListOf<Int>().apply { repeat(TRACKS * SCENES) { add(1) } }
    val warp = mutableStateListOf<Boolean>().apply { repeat(TRACKS * SCENES) { add(true) } }
    val transpose = mutableStateListOf<Int>().apply { repeat(TRACKS * SCENES) { add(0) } }
    val frames = mutableStateListOf<Int>().apply { repeat(TRACKS * SCENES) { add(0) } }

    private val taps = ArrayDeque<Long>()

    fun idx(t: Int, s: Int) = t * SCENES + s

    fun refreshSlot(t: Int, s: Int) {
        val i = idx(t, s)
        val info = IntArray(5)
        NativeEngine.nativeClipInfo(t, s, info)
        has[i] = info[0] == 1
        bars[i] = info[1]
        warp[i] = info[2] == 1
        transpose[i] = info[3]
        frames[i] = info[4]
        names[i] = NativeEngine.nativeClipName(t, s)
    }

    fun pushClip(t: Int, s: Int) {
        val i = idx(t, s)
        NativeEngine.nativeSetClipParams(t, s, if (warp[i]) 1 else 0, bars[i], transpose[i])
    }

    fun setBpm(v: Float) {
        bpm = v.coerceIn(60f, 200f)
        NativeEngine.nativeSetGlobal(NativeEngine.GP_BPM, bpm)
    }

    /** TAP tempo: average interval of the last 4 taps. */
    fun tap() {
        val now = SystemClock.elapsedRealtime()
        if (taps.isNotEmpty() && now - taps.last() > 2000) taps.clear()
        taps.addLast(now)
        while (taps.size > 4) taps.removeFirst()
        if (taps.size >= 2) {
            val intervals = taps.zipWithNext { a, b -> (b - a).toDouble() }
            setBpm((60000.0 / intervals.average()).toFloat())
        }
    }

    fun pushAll() {
        NativeEngine.nativeSetGlobal(NativeEngine.GP_BPM, bpm)
        NativeEngine.nativeSetGlobal(NativeEngine.GP_QUANT, quantIdx.toFloat())
        NativeEngine.nativeSetGlobal(NativeEngine.GP_METRONOME, if (metronome) 1f else 0f)
        NativeEngine.nativeSetGlobal(NativeEngine.GP_CROSSFADER, xfader)
        NativeEngine.nativeSetGlobal(NativeEngine.GP_MASTER_VOL, faderToLin(masterFader))
        NativeEngine.nativeSetGlobal(NativeEngine.GP_RET_A_VOL, faderToLin(retAFader))
        NativeEngine.nativeSetGlobal(NativeEngine.GP_RET_B_VOL, faderToLin(retBFader))
        NativeEngine.nativeSetGlobal(NativeEngine.GP_DELAY_MODE, delayMode.toFloat())
        NativeEngine.nativeSetGlobal(NativeEngine.GP_CLICK_VOL, clickVol)
        for (t in 0 until TRACKS) {
            pushTrack(t)
            for (s in 0 until SCENES) refreshSlot(t, s)
        }
    }

    fun pushTrack(t: Int) {
        val u = tracks[t]
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_VOLUME, faderToLin(u.fader))
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_PAN, u.pan)
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_SEND_A, u.sendA * u.sendA)
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_SEND_B, u.sendB * u.sendB)
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_ACTIVE, if (u.active) 1f else 0f)
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_SOLO, if (u.solo) 1f else 0f)
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_XF_ASSIGN, u.xf.toFloat())
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_FILTER_TYPE, u.filterType.toFloat())
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_CUTOFF, u.cutoffHz())
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_RESONANCE, u.resQ())
        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_DRIVE, u.drive)
    }
}

// ------------------------------------------------------------------ small controls
@Composable
private fun TinyButton(
    text: String, active: Boolean, activeColor: Color, modifier: Modifier = Modifier,
    inactiveText: Color = TextDim, activeText: Color = Color(0xFF111111), onClick: () -> Unit
) {
    Box(
        modifier
            .background(if (active) activeColor else CellBg)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center
    ) {
        Text(text, color = if (active) activeText else inactiveText, fontSize = 10.sp,
            fontWeight = FontWeight.Bold, maxLines = 1)
    }
}

/** Horizontal slider, value 0..1. Double-tap resets to [default]. */
@Composable
private fun HSlider(
    value: Float, label: String, color: Color, modifier: Modifier = Modifier,
    bipolar: Boolean = false, default: Float = 0f, onChange: (Float) -> Unit
) {
    Box(
        modifier
            .background(CellBg)
            .pointerInput(Unit) {
                detectTapGestures(
                    onDoubleTap = { onChange(default) },
                    onTap = { onChange((it.x / size.width).coerceIn(0f, 1f)) })
            }
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragStart = { onChange((it.x / size.width).coerceIn(0f, 1f)) },
                    onDrag = { change, _ -> onChange((change.position.x / size.width).coerceIn(0f, 1f)) })
            }
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val v = value.coerceIn(0f, 1f)
            if (bipolar) {
                val c = size.width * 0.5f
                val x = size.width * v
                drawRect(color, Offset(minOf(c, x), 0f), Size(kotlin.math.abs(x - c), size.height))
            } else {
                drawRect(color.copy(alpha = 0.85f), Offset(0f, 0f), Size(size.width * v, size.height))
            }
        }
        Text(label, color = TextMain, fontSize = 9.sp, maxLines = 1, overflow = TextOverflow.Clip,
            modifier = Modifier.align(Alignment.CenterStart).padding(start = 3.dp))
    }
}

/** Vertical volume fader (dB scale). */
@Composable
private fun VFader(pos: Float, modifier: Modifier = Modifier, onChange: (Float) -> Unit) {
    Canvas(
        modifier
            .background(CellBg)
            .pointerInput(Unit) {
                detectTapGestures(
                    onDoubleTap = { onChange(UNITY_FADER) },
                    onTap = { onChange((1f - it.y / size.height).coerceIn(0f, 1f)) })
            }
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragStart = { onChange((1f - it.y / size.height).coerceIn(0f, 1f)) },
                    onDrag = { change, _ -> onChange((1f - change.position.y / size.height).coerceIn(0f, 1f)) })
            }
    ) {
        val unityY = size.height * (1f - UNITY_FADER)
        drawRect(Color(0xFF444444), Offset(0f, unityY), Size(size.width, 1.5f))
        val y = size.height * (1f - pos.coerceIn(0f, 1f))
        drawRect(Color(0xFF1A1A1A), Offset(size.width * 0.42f, 0f), Size(size.width * 0.16f, size.height))
        drawRect(Color(0xFFBDBDBD), Offset(0f, (y - 4f).coerceIn(0f, size.height - 8f)), Size(size.width, 8f))
        drawRect(Accent, Offset(0f, (y - 1f).coerceIn(0f, size.height - 2f)), Size(size.width, 2f))
    }
}

/** Stereo peak/RMS meter; reads state only in the draw phase (no recomposition per frame). */
@Composable
private fun LevelMeter(m: MeterState, modifier: Modifier = Modifier) {
    Canvas(modifier.background(Color(0xFF181818))) {
        val gradient = Brush.verticalGradient(
            0f to RecRed, 0.1f to Color(0xFFFFC107), 0.35f to QueueGreen, 1f to Color(0xFF1E8E52))
        val barW = size.width / 2f - 1f
        val rms = floatArrayOf(m.rmsL, m.rmsR)
        val peak = floatArrayOf(m.peakL, m.peakR)
        for (ch in 0..1) {
            val x = ch * (barW + 2f)
            val h = size.height * meterNorm(rms[ch])
            clipRect(x, size.height - h, x + barW, size.height) {
                drawRect(gradient, Offset(x, 0f), Size(barW, size.height))
            }
            val py = size.height * (1f - meterNorm(peak[ch]))
            drawRect(Color(0xFFEDEDED), Offset(x, py.coerceIn(0f, size.height - 2f)), Size(barW, 2f))
        }
    }
}

@Composable
private fun PeakText(m: MeterState) {
    Text(
        linToDbText(maxOf(m.peakL, m.peakR)), color = TextDim, fontSize = 8.sp, maxLines = 1,
        textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth()
    )
}

// ------------------------------------------------------------------ session grid
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ClipCell(ui: DawState, t: Int, s: Int, modifier: Modifier, onLoad: (Int, Int) -> Unit) {
    val i = ui.idx(t, s)
    val has = ui.has[i]
    val st = ui.states[i]
    val tc = TrackColors[t]
    val blinking = st == 2 || st == 3 || st == 5 || st == 6
    val blink = if (blinking) ui.blink else false
    val bg = when (st) {
        1 -> tc
        2 -> if (blink) QueueGreen else tc.copy(alpha = 0.45f)
        3 -> if (blink) tc.copy(alpha = 0.55f) else tc
        4 -> RecRed
        5, 6 -> if (blink) RecRed else RecRed.copy(alpha = 0.35f)
        else -> if (has) tc.copy(alpha = 0.42f) else EmptyCell
    }
    val bright = st == 1 || st == 3 || st == 4 || (st == 2 && blink) || ((st == 5 || st == 6) && blink)
    val fg = if (bright) Color(0xFF111111) else TextMain
    val selected = ui.selT == t && ui.selS == s
    val armed = ui.tracks[t].armed
    Box(
        modifier
            .background(bg)
            .then(if (selected) Modifier.border(1.dp, Color.White.copy(alpha = 0.85f)) else Modifier)
            .combinedClickable(
                onClick = {
                    ui.selT = t; ui.selS = s
                    NativeEngine.nativeSlotPress(t, s)
                },
                onLongClick = {
                    ui.selT = t; ui.selS = s
                    onLoad(t, s)
                })
    ) {
        Row(Modifier.fillMaxSize(), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(20.dp).fillMaxHeight(), contentAlignment = Alignment.Center) {
                val icon = when {
                    st == 4 || st == 6 -> "●"
                    has -> "▶"
                    armed -> "●"
                    else -> "■"
                }
                val iconColor = if (!has && armed) RecRed else if (has) fg else TextDim.copy(alpha = 0.7f)
                Text(icon, color = iconColor, fontSize = 11.sp)
            }
            if (has) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.Center) {
                    Text(ui.names[i], color = fg, fontSize = 10.sp, fontWeight = FontWeight.Bold,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                    val fr = ui.frames[i]
                    val nat = if (fr > 0) (ui.bars[i] * 4f * 60f * 48000f / fr).roundToInt() else 0
                    Text("${ui.bars[i]} bar${if (ui.bars[i] > 1) "s" else ""} · $nat bpm",
                        color = fg.copy(alpha = 0.75f), fontSize = 8.sp, maxLines = 1)
                }
            }
        }
        if (st == 1 || st == 3 || st == 4 || st == 6) {
            val prog = ui.progress[t]
            Box(
                Modifier.align(Alignment.BottomStart).fillMaxWidth(prog.coerceIn(0f, 1f)).height(3.dp)
                    .background(Color.White.copy(alpha = 0.9f))
            )
        }
    }
}

@Composable
private fun HeaderCell(text: String, color: Color, modifier: Modifier) {
    Box(modifier.background(Panel), contentAlignment = Alignment.CenterStart) {
        Box(Modifier.width(3.dp).fillMaxHeight().background(color))
        Text(text, color = TextMain, fontSize = 10.sp, fontWeight = FontWeight.Bold, maxLines = 1,
            modifier = Modifier.padding(start = 6.dp))
    }
}

// ------------------------------------------------------------------ mixer strips
@Composable
private fun FaderBlock(fader: Float, meter: MeterState, modifier: Modifier, onChange: (Float) -> Unit) {
    Column(modifier.fillMaxHeight()) {
        PeakText(meter)
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
            VFader(fader, Modifier.weight(1f).fillMaxHeight(), onChange)
            LevelMeter(meter, Modifier.width(10.dp).fillMaxHeight())
        }
        Text(faderDbText(fader), color = TextDim, fontSize = 8.sp, textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth())
    }
}

@Composable
private fun TrackStrip(ui: DawState, t: Int, modifier: Modifier) {
    val u = ui.tracks[t]
    val col = TrackColors[t]
    Row(modifier.background(Panel).padding(2.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            HSlider(u.sendA, "Send A", Accent, Modifier.fillMaxWidth().height(17.dp)) {
                u.sendA = it
                NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_SEND_A, it * it)
            }
            HSlider(u.sendB, "Send B", Color(0xFF58C4DD), Modifier.fillMaxWidth().height(17.dp)) {
                u.sendB = it
                NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_SEND_B, it * it)
            }
            val panTxt = when {
                kotlin.math.abs(u.pan) < 0.02f -> "Pan C"
                u.pan < 0 -> "Pan L${(-u.pan * 50).roundToInt()}"
                else -> "Pan R${(u.pan * 50).roundToInt()}"
            }
            HSlider((u.pan + 1f) / 2f, panTxt, col, Modifier.fillMaxWidth().height(17.dp),
                bipolar = true, default = 0.5f) {
                u.pan = it * 2f - 1f
                NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_PAN, u.pan)
            }
            Row(Modifier.fillMaxWidth().height(24.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                TinyButton("${t + 1}", u.active, Yellow, Modifier.weight(1f).fillMaxHeight()) {
                    u.active = !u.active
                    NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_ACTIVE, if (u.active) 1f else 0f)
                }
                TinyButton("S", u.solo, SoloBlue, Modifier.weight(1f).fillMaxHeight()) {
                    val on = !u.solo
                    if (on) ui.tracks.forEachIndexed { k, o ->
                        if (k != t && o.solo) { o.solo = false; NativeEngine.nativeSetTrackParam(k, NativeEngine.TP_SOLO, 0f) }
                    }
                    u.solo = on
                    NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_SOLO, if (on) 1f else 0f)
                }
                TinyButton("●", u.armed, RecRed, Modifier.weight(1f).fillMaxHeight(), activeText = Color.White) {
                    u.armed = !u.armed
                    NativeEngine.nativeSetArm(t, u.armed)
                }
            }
            Row(Modifier.fillMaxWidth().height(20.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                listOf("A", "-", "B").forEachIndexed { k, label ->
                    TinyButton(label, u.xf == k, if (k == 1) Color(0xFF666666) else Accent,
                        Modifier.weight(1f).fillMaxHeight(), activeText = Color.White) {
                        u.xf = k
                        NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_XF_ASSIGN, k.toFloat())
                    }
                }
            }
        }
        FaderBlock(u.fader, ui.meters[t], Modifier.width(34.dp)) {
            u.fader = it
            NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_VOLUME, faderToLin(it))
        }
    }
}

@Composable
private fun ReturnStrip(ui: DawState, isA: Boolean, modifier: Modifier) {
    val fader = if (isA) ui.retAFader else ui.retBFader
    Row(modifier.background(Panel).padding(2.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(if (isA) "Reverb" else "Ping-Pong", color = TextMain, fontSize = 10.sp, fontWeight = FontWeight.Bold)
            Text(if (isA) "Stereo room" else "Tempo synced", color = TextDim, fontSize = 8.sp)
            if (!isA) {
                Row(Modifier.fillMaxWidth().height(22.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                    DelayLabels.forEachIndexed { k, label ->
                        TinyButton(label, ui.delayMode == k, Accent, Modifier.weight(1f).fillMaxHeight()) {
                            ui.delayMode = k
                            NativeEngine.nativeSetGlobal(NativeEngine.GP_DELAY_MODE, k.toFloat())
                        }
                    }
                }
            }
        }
        FaderBlock(fader, ui.meters[if (isA) 4 else 5], Modifier.width(34.dp)) {
            if (isA) {
                ui.retAFader = it
                NativeEngine.nativeSetGlobal(NativeEngine.GP_RET_A_VOL, faderToLin(it))
            } else {
                ui.retBFader = it
                NativeEngine.nativeSetGlobal(NativeEngine.GP_RET_B_VOL, faderToLin(it))
            }
        }
    }
}

@Composable
private fun MasterStrip(ui: DawState, modifier: Modifier) {
    Row(modifier.background(Panel).padding(2.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            val xTxt = when {
                ui.xfader < 0.45f -> "A ◄ xfade"
                ui.xfader > 0.55f -> "xfade ► B"
                else -> "A ◄ ► B"
            }
            HSlider(ui.xfader, xTxt, Accent, Modifier.fillMaxWidth().height(20.dp), bipolar = true, default = 0.5f) {
                ui.xfader = it
                NativeEngine.nativeSetGlobal(NativeEngine.GP_CROSSFADER, it)
            }
            HSlider(ui.clickVol, "Cue/Click", Color(0xFFB0B0B0), Modifier.fillMaxWidth().height(17.dp), default = 0.5f) {
                ui.clickVol = it
                NativeEngine.nativeSetGlobal(NativeEngine.GP_CLICK_VOL, it)
            }
            Text("Limiter ON", color = TextDim, fontSize = 8.sp)
        }
        FaderBlock(ui.masterFader, ui.meters[6], Modifier.width(34.dp)) {
            ui.masterFader = it
            NativeEngine.nativeSetGlobal(NativeEngine.GP_MASTER_VOL, faderToLin(it))
        }
    }
}

// ------------------------------------------------------------------ side panel & top bar
@Composable
private fun SectionTitle(text: String) {
    Text(text, color = TextDim, fontSize = 9.sp, fontWeight = FontWeight.Bold, modifier = Modifier.padding(top = 2.dp))
}

@Composable
private fun SidePanel(ui: DawState, modifier: Modifier, onLoad: (Int, Int) -> Unit) {
    val t = ui.selT
    val s = ui.selS
    val i = ui.idx(t, s)
    val u = ui.tracks[t]
    Column(
        modifier.background(Panel).padding(6.dp).verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        SectionTitle("CLIP")
        Text(if (ui.has[i]) ui.names[i] else "Empty slot", color = TrackColors[t], fontSize = 12.sp,
            fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text("Track ${t + 1} · Scene ${s + 1}", color = TextDim, fontSize = 9.sp)
        TinyButton("LOAD SAMPLE…", false, Accent, Modifier.fillMaxWidth().height(24.dp), inactiveText = Accent) { onLoad(t, s) }
        if (ui.has[i]) {
            Row(Modifier.fillMaxWidth().height(24.dp), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Warp", color = TextMain, fontSize = 10.sp, modifier = Modifier.weight(1f))
                TinyButton(if (ui.warp[i]) "ON" else "OFF", ui.warp[i], Accent, Modifier.width(56.dp).fillMaxHeight()) {
                    ui.warp[i] = !ui.warp[i]
                    ui.pushClip(t, s)
                }
            }
            Text("Length (bars)", color = TextMain, fontSize = 10.sp)
            Row(Modifier.fillMaxWidth().height(22.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                listOf(1, 2, 4, 8).forEach { b ->
                    TinyButton("$b", ui.bars[i] == b, Accent, Modifier.weight(1f).fillMaxHeight()) {
                        ui.bars[i] = b
                        ui.pushClip(t, s)
                    }
                }
            }
            Row(Modifier.fillMaxWidth().height(24.dp), verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                Text("Transpose", color = TextMain, fontSize = 10.sp, modifier = Modifier.weight(1f))
                TinyButton("−", false, Accent, Modifier.width(26.dp).fillMaxHeight()) {
                    ui.transpose[i] = (ui.transpose[i] - 1).coerceAtLeast(-12)
                    ui.pushClip(t, s)
                }
                Text("${if (ui.transpose[i] > 0) "+" else ""}${ui.transpose[i]} st", color = TextMain, fontSize = 10.sp,
                    textAlign = TextAlign.Center, modifier = Modifier.width(40.dp))
                TinyButton("+", false, Accent, Modifier.width(26.dp).fillMaxHeight()) {
                    ui.transpose[i] = (ui.transpose[i] + 1).coerceAtMost(12)
                    ui.pushClip(t, s)
                }
            }
        }
        SectionTitle("TRACK ${t + 1} · CHANNEL STRIP")
        Row(Modifier.fillMaxWidth().height(22.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
            listOf("Off", "LP", "HP").forEachIndexed { k, label ->
                TinyButton(label, u.filterType == k, Accent, Modifier.weight(1f).fillMaxHeight()) {
                    u.filterType = k
                    NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_FILTER_TYPE, k.toFloat())
                }
            }
        }
        val hz = u.cutoffHz()
        HSlider(u.cutoffPos, "Cutoff " + (if (hz >= 1000f) String.format(Locale.US, "%.1fk", hz / 1000f) else "${hz.roundToInt()} Hz"),
            Accent, Modifier.fillMaxWidth().height(20.dp), default = 0.75f) {
            u.cutoffPos = it
            NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_CUTOFF, u.cutoffHz())
        }
        HSlider(u.resPos, "Reso Q " + String.format(Locale.US, "%.1f", u.resQ()), Accent,
            Modifier.fillMaxWidth().height(20.dp), default = 0.2f) {
            u.resPos = it
            NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_RESONANCE, u.resQ())
        }
        HSlider(u.drive, "Drive " + (u.drive * 100).roundToInt() + "%", Color(0xFFE5578D),
            Modifier.fillMaxWidth().height(20.dp)) {
            u.drive = it
            NativeEngine.nativeSetTrackParam(t, NativeEngine.TP_DRIVE, it)
        }
    }
}

@Composable
private fun TopBar(ui: DawState, modifier: Modifier) {
    var menuOpen by remember { mutableStateOf(false) }
    Row(
        modifier.background(Panel).padding(horizontal = 6.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        TinyButton("▶", ui.playing, QueueGreen, Modifier.width(44.dp).fillMaxHeight()) { NativeEngine.nativePlay() }
        TinyButton("■", false, Accent, Modifier.width(44.dp).fillMaxHeight(), inactiveText = TextMain) {
            NativeEngine.nativeStopTransport()
        }
        TinyButton("TAP", false, Accent, Modifier.width(46.dp).fillMaxHeight(), inactiveText = TextMain) { ui.tap() }
        TinyButton("−", false, Accent, Modifier.width(24.dp).fillMaxHeight(), inactiveText = TextMain) { ui.setBpm(ui.bpm - 1f) }
        Box(
            Modifier.width(66.dp).fillMaxHeight().background(Color(0xFF181818))
                .pointerInput(Unit) {
                    detectDragGestures { change, drag ->
                        change.consume()
                        ui.setBpm(((ui.bpm - drag.y / 6f) * 10f).roundToInt() / 10f)
                    }
                },
            contentAlignment = Alignment.Center
        ) {
            Text(String.format(Locale.US, "%.1f", ui.bpm), color = Accent, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        }
        TinyButton("+", false, Accent, Modifier.width(24.dp).fillMaxHeight(), inactiveText = TextMain) { ui.setBpm(ui.bpm + 1f) }
        Box(Modifier.width(92.dp).fillMaxHeight().background(Color(0xFF181818)), contentAlignment = Alignment.Center) {
            Text(ui.position, color = TextMain, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        }
        TinyButton("CLICK", ui.metronome, Accent, Modifier.width(52.dp).fillMaxHeight()) {
            ui.metronome = !ui.metronome
            NativeEngine.nativeSetGlobal(NativeEngine.GP_METRONOME, if (ui.metronome) 1f else 0f)
        }
        Box {
            Box(
                Modifier.width(108.dp).fillMaxHeight().background(CellBg).clickable { menuOpen = true },
                contentAlignment = Alignment.Center
            ) { Text("Q: ${QuantLabels[ui.quantIdx]}", color = TextMain, fontSize = 10.sp, fontWeight = FontWeight.Bold) }
            DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
                QuantLabels.forEachIndexed { k, label ->
                    DropdownMenuItem(text = { Text(label) }, onClick = {
                        ui.quantIdx = k
                        NativeEngine.nativeSetGlobal(NativeEngine.GP_QUANT, k.toFloat())
                        menuOpen = false
                    })
                }
            }
        }
        Box(Modifier.weight(1f).fillMaxHeight(), contentAlignment = Alignment.CenterEnd) {
            Text(if (ui.loading) "Decoding…" else "MINI LIVE 11", color = if (ui.loading) Accent else TextDim,
                fontSize = 10.sp, fontWeight = FontWeight.Bold)
        }
    }
}

// ------------------------------------------------------------------ root screen
@Composable
private fun DawRoot(ui: DawState, onLoad: (Int, Int) -> Unit) {
    LaunchedEffect(Unit) {
        val st = FloatArray(64)
        val me = FloatArray(28)
        val ev = IntArray(5)
        var tick = 0
        while (isActive) {
            NativeEngine.nativeGetStatus(st)
            NativeEngine.nativeGetMeters(me)
            val playing = st[0] > 0.5f
            if (ui.playing != playing) ui.playing = playing
            val pos = String.format(Locale.US, "%03d.%d.%d", st[2].toInt(), st[3].toInt(), st[4].toInt())
            if (ui.position != pos) ui.position = pos
            for (k in 0 until TRACKS * SCENES) {
                val v = st[8 + k].toInt()
                if (ui.states[k] != v) ui.states[k] = v
            }
            for (t in 0 until TRACKS) {
                val p = st[28 + t]
                if (ui.progress[t] != p) ui.progress[t] = p
                val q = st[36 + t] > 0.5f
                if (ui.stopQueued[t] != q) ui.stopQueued[t] = q
            }
            for (c in 0 until 7) {
                val m = ui.meters[c]
                m.peakL = me[c * 4]; m.peakR = me[c * 4 + 1]
                m.rmsL = me[c * 4 + 2]; m.rmsR = me[c * 4 + 3]
            }
            while (NativeEngine.nativePollEvent(ev)) {
                if (ev[0] == 1) ui.refreshSlot(ev[1], ev[2])   // live-loop recording finished
            }
            tick++
            if (tick % 8 == 0) ui.blink = !ui.blink
            delay(33)
        }
    }

    Column(Modifier.fillMaxSize().background(BgDark).padding(4.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
        TopBar(ui, Modifier.fillMaxWidth().height(40.dp))
        Row(Modifier.weight(1f).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(3.dp)) {
            Column(Modifier.weight(1f).fillMaxHeight(), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                // headers
                Row(Modifier.fillMaxWidth().height(18.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                    for (t in 0 until TRACKS) HeaderCell("${t + 1} Audio", TrackColors[t], Modifier.weight(1f).fillMaxHeight())
                    HeaderCell("A Reverb", Color(0xFF8C8C8C), Modifier.weight(1f).fillMaxHeight())
                    HeaderCell("B Delay", Color(0xFF8C8C8C), Modifier.weight(1f).fillMaxHeight())
                    HeaderCell("Master", Accent, Modifier.weight(1f).fillMaxHeight())
                }
                // clip grid + scene launch column
                for (s in 0 until SCENES) {
                    Row(Modifier.fillMaxWidth().height(30.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                        for (t in 0 until TRACKS) ClipCell(ui, t, s, Modifier.weight(1f).fillMaxHeight(), onLoad)
                        Box(Modifier.weight(1f).fillMaxHeight().background(EmptyCell))
                        Box(Modifier.weight(1f).fillMaxHeight().background(EmptyCell))
                        Box(
                            Modifier.weight(1f).fillMaxHeight().background(CellBg)
                                .clickable { NativeEngine.nativeLaunchScene(s) },
                            contentAlignment = Alignment.Center
                        ) { Text("▶  Scene ${s + 1}", color = Accent, fontSize = 10.sp, fontWeight = FontWeight.Bold) }
                    }
                }
                // stop row
                Row(Modifier.fillMaxWidth().height(24.dp), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                    for (t in 0 until TRACKS) {
                        val blink = ui.stopQueued[t] && ui.blink
                        TinyButton("■", blink, Accent, Modifier.weight(1f).fillMaxHeight(), inactiveText = TextMain) {
                            NativeEngine.nativeStopTrack(t)
                        }
                    }
                    Box(Modifier.weight(1f).fillMaxHeight().background(EmptyCell))
                    Box(Modifier.weight(1f).fillMaxHeight().background(EmptyCell))
                    TinyButton("Stop All Clips", false, Accent, Modifier.weight(1f).fillMaxHeight(), inactiveText = TextMain) {
                        NativeEngine.nativeStopAll()
                    }
                }
                // mixer
                Row(Modifier.fillMaxWidth().weight(1f), horizontalArrangement = Arrangement.spacedBy(2.dp)) {
                    for (t in 0 until TRACKS) TrackStrip(ui, t, Modifier.weight(1f).fillMaxHeight())
                    ReturnStrip(ui, true, Modifier.weight(1f).fillMaxHeight())
                    ReturnStrip(ui, false, Modifier.weight(1f).fillMaxHeight())
                    MasterStrip(ui, Modifier.weight(1f).fillMaxHeight())
                }
            }
            SidePanel(ui, Modifier.width(190.dp).fillMaxHeight(), onLoad)
        }
    }
}

// ------------------------------------------------------------------ activity
class MainActivity : ComponentActivity() {
    private val ui = DawState()
    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { startEngine() }

    private fun startEngine() {
        if (!NativeEngine.nativeStart()) {
            Toast.makeText(this, "Audio engine failed to start", Toast.LENGTH_LONG).show()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        NativeEngine.nativeCreate()
        ui.pushAll()

        setContent {
            MaterialTheme(colorScheme = darkColorScheme(primary = Accent, background = BgDark, surface = Panel)) {
                val context = LocalContext.current
                val scope = rememberCoroutineScope()
                var target by remember { mutableStateOf(Pair(0, 0)) }
                val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
                    if (uri != null) {
                        val (t, s) = target
                        scope.launch {
                            ui.loading = true
                            try {
                                val audio = AudioDecoder.decode(context, uri)
                                if (NativeEngine.nativeLoadClip(t, s, audio.data, audio.frames, audio.name)) {
                                    ui.refreshSlot(t, s)
                                    ui.selT = t; ui.selS = s
                                } else {
                                    Toast.makeText(context, "Sample too short", Toast.LENGTH_SHORT).show()
                                }
                            } catch (e: Exception) {
                                Toast.makeText(context, "Cannot decode: ${e.message}", Toast.LENGTH_LONG).show()
                            } finally {
                                ui.loading = false
                            }
                        }
                    }
                }
                DawRoot(ui) { t, s ->
                    target = Pair(t, s)
                    picker.launch(arrayOf("audio/*"))
                }
            }
        }
        permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
    }

    override fun onDestroy() {
        NativeEngine.nativeStop()
        if (isFinishing) NativeEngine.nativeDestroy()
        super.onDestroy()
    }
}
